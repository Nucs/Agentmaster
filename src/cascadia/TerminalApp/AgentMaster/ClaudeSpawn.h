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
#include <optional>
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
    // modelOverride (Agentmaster, launch-model picker): non-empty => append ` --model <id>` — the
    //   per-LAUNCH model picked from an "Open New Session Here" submenu. A CLI flag outranks the
    //   shared settings file, so this overrides the cog's global `model` for THIS session only;
    //   the Fleet Observer reads it back off the live commandline (ReadClaudeFacts), so the card /
    //   overlay `model·effort` adornment shows the pick with no extra plumbing. Quoted iff it
    //   contains whitespace. Empty => no flag (exactly the pre-picker commandline).
    // initialPrompt (Agentmaster, COMMANDS.md — the /handover successor): non-empty => append it as
    //   the trailing POSITIONAL prompt argument (`claude [flags] "<prompt>"`), which claude submits
    //   as the session's FIRST turn on startup — the zero-race way to hand a new session its opening
    //   message (no stdin injection, no TUI-init Enter-eaten window; the prompt fires a real
    //   UserPromptSubmit hook, so state/record ride the normal push path). PS-quoted via
    //   PsDoubleQuote — the managed commandline is invoked by the pwsh host's `&` operator
    //   (BuildPwshHostedCommandline), so PowerShell parsing rules govern the arg. Empty => no arg
    //   (byte-identical to the pre-parameter commandline).
    std::wstring BuildClaudeCommandline(std::wstring_view settingsPath, std::wstring_view sessionId, bool resume, bool skipPermissions, std::wstring_view forkFromSessionId = {}, std::wstring_view claudeLauncher = {}, std::wstring_view modelOverride = {}, std::wstring_view initialPrompt = {});

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

    // Agentmaster (COMMANDS.md): quote a string as a PowerShell DOUBLE-quoted argument — "text"
    // with the three characters PS interprets inside double quotes backtick-escaped (` -> ``,
    // " -> `", $ -> `$). Used for the launch commandline's initial-prompt positional arg: the
    // managed commandline runs under the pwsh host's `&` operator (BuildPwshHostedCommandline),
    // so PS parsing — not CreateProcessW — governs its args, and a path carrying $ or ` must not
    // expand. (PsSingleQuote is unsuitable here only by convention — the commandline's existing
    // args are all double-quoted, and mixing styles in one line reads worse in Copy-Launch-CLI.)
    // Pure + unit-tested.
    std::wstring PsDoubleQuote(std::wstring_view s);

    // Agentmaster (COMMANDS.md — /handover content injection). The COMMANDLINE-TIER threshold, in
    // POST-PsDoubleQuote-escape characters — NOT a truncation bound (the message is NEVER
    // truncated): content whose escaped cost fits rides the launch commandline's positional
    // prompt (the zero-race channel); anything larger is delivered IN FULL through the ConPTY
    // stdin as a bracketed PASTE (BuildPromptSubmission via the queue/injector machinery — a
    // streamed pipe with no size ceiling). Why the commandline caps: the prompt crosses TWO
    // CreateProcessW hops, each hard-capped at 32,767 chars — and the BINDING one is the outer
    // pwsh `-EncodedCommand <base64(UTF-16LE("& <inner>"))>` wrap: base64 costs 4·ceil(2·I/3) ≈
    // 2.67·I chars for an inner commandline of I wchars, so I ≤ ~12,200; minus the inner's
    // non-prompt overhead (claude full path + --settings full path + flags + the session guid,
    // ~300-700 depending on profile paths) that leaves ~11,500 escaped chars for the quoted
    // prompt — the constant, with margin. Escaped chars ≈ raw chars + one per ` " $ (PsDoubleQuote
    // doubles exactly those three), which is why the tier check is escape-aware, not raw-length.
    inline constexpr size_t kHandoverPromptEscapedBudget = 11'500;

    // PURE: `text`'s POST-PsDoubleQuote-escape character cost (the tier check's input — the
    // PsDoubleQuote cost model: ` " $ cost 2, everything else 1). Unit-tested against the REAL
    // PsDoubleQuote output size so the model can't drift.
    size_t PsEscapedCost(std::wstring_view text);

    // Agentmaster (COMMANDS.md — /handover content injection). Read the handover markdown and
    // shape it into the successor's FIRST USER MESSAGE — the file's content VERBATIM and IN FULL
    // ("as if the user typed it"; NEVER truncated — the caller picks the delivery channel by
    // PsEscapedCost: commandline under the tier threshold, bracketed-paste stdin injection
    // above it), normalized only (UTF-8 BOM stripped, CRLF -> LF, other C0 control chars except
    // \n/\t dropped — never legitimate markdown, and they would gunk the PS arg / paste framing +
    // the transcript; outer whitespace trimmed). Bounded read (4 MiB cap — a sanity ceiling, not
    // a message bound; a document beyond it is not a handover briefing). Returns "" on a
    // missing/unreadable/empty/beyond-cap file — the caller falls back to the pointer-style
    // prompt so the handover still functions and NOTHING is ever silently cut.
    std::wstring ReadHandoverDocumentPrompt(const std::wstring& mdPath);

    // Parse a list of NAME=VALUE assignments into pairs, for AppSettings.env (extra environment
    // applied to every spawned session) and the per-directory env (dir-env.json). Entries are
    // separated by EITHER a newline (the multi-line editor format) OR a ';' (the legacy single-line
    // format), so an old ';'-delimited settings.json still parses. Entries without '=' / with an
    // empty NAME / starting with '#' (a comment) are skipped; whitespace around an entry and around
    // NAME is trimmed; VALUE is taken verbatim (may itself contain '='). Pure + unit-tested.
    std::vector<std::pair<std::wstring, std::wstring>> ParseEnvAssignments(std::wstring_view spec);

    // Agentmaster (launch-model picker): parse AppSettings.launchModels — the "Display name |
    // model-id" list behind every "Open New Session Here" model submenu — into ordered
    // {displayName, modelId} pairs. Entries are separated by newline OR ';' (the same lenient
    // splitting as ParseEnvAssignments, so the multi-line cog editor and a hand-edited single
    // line both parse); the FIRST '|' splits name from id, both sides trimmed. A bare entry with
    // no '|' uses the token as BOTH name and id (a raw model id still lists); blank entries and
    // '#' comments are skipped, as is an entry whose name or id trims to empty. Order is
    // preserved (the submenu lists top-to-bottom as typed; duplicates are kept verbatim — the
    // user's list, not ours to dedupe). Capped at kMaxLaunchModels so a runaway settings edit
    // can't balloon every context menu. Pure + unit-tested.
    inline constexpr size_t kMaxLaunchModels = 32;
    std::vector<std::pair<std::wstring, std::wstring>> ParseLaunchModels(std::wstring_view spec);

    // Agentmaster (launch-model picker): is `spec` still one of the launch-model lists we USED to
    // ship as the default (kSupersededLaunchModels, SessionModels.h) — i.e. a list the user never
    // edited? The settings load upgrades such a spec to kDefaultLaunchModels, so an existing
    // install follows a default change instead of being pinned to it forever by the key's mere
    // presence; an edited list (any rename/reorder/added/removed entry) and a deliberately EMPTY
    // one ("no models — just Default") are never touched. Compares the PARSED {name, id} pairs,
    // not the bytes, so the cog TextBox's '\r' line endings, a trailing blank line, and spacing
    // around the '|' don't hide a pristine list. PURE + unit-tested.
    bool LaunchModelsAreSupersededDefault(std::wstring_view spec);

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

    // Agentmaster (launch-model picker): the launch-models twin of LexEnvText — a per-entry verdict
    // over the Settings cog's "Launch models" editor, driving the SAME live border color + bottom
    // status line the env editors have. REUSES the EnvLex* result shapes (they are shape-generic:
    // line/kind/name/message + counts/worst/firstIssue); `EnvLineDiag::name` carries the entry's
    // DISPLAY NAME. Verdicts mirror ParseLaunchModels EXACTLY:
    //   Ignored — blank / '#' comment;
    //   Ok      — a listed "Display name | model-id" (a bare token — its own label — is Ok too);
    //   Warn    — LISTED but noteworthy: a duplicate display name (both DO list — a confusing menu;
    //             ASCII-case-insensitive like the fold users perceive), or a model id carrying
    //             whitespace (it launches quoted as --model "…" — almost certainly a typo) — plus
    //             the NOT-listed case: any entry past the kMaxLaunchModels cap (parsed but DROPPED);
    //   Error   — a '|' entry whose display-name or model-id side trims to empty (SKIPPED).
    // Unlike LexEnvText, `ok` counts every entry that WILL be offered (Ok + the listed Warns, NOT
    // the past-cap drops), so the status line's "N models" is the true submenu size. Entries are
    // split like the parser (';'/'\r' inside a '\n' line too — each lexes as its own entry carrying
    // that line's number), so lexer and parser can never disagree on what applies. PURE + unit-tested.
    EnvLexResult LexLaunchModelsText(std::wstring_view text);

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

    // ── Exception forensics (the "never lose a swallowed exception" policy) ────────────────────
    // A swallowed catch(...) must not discard WHAT was thrown nor WHERE FROM. The catch site runs
    // AFTER the unwind (the throw-site stack is gone by then), so a process-wide Vectored Exception
    // Handler captures the raw stack AT RAISE TIME (VEH runs before unwinding) for every MSVC C++
    // throw (0xE06D7363) into a per-thread ring — rethrows (e.g. a coroutine's stored exception
    // re-raised at co_await) capture again, so the ring's older entries keep the ORIGINAL throw.
    // Frames are logged as `Module.dll+0xRVA` — offline-symbolizable against the build's PDBs with
    // the debug-dumps diasym tool (ASLR-stable), so a hooks.log line alone pins file:line later.

    // Install the VEH throw-stack capture. Process-once (internal call_once), cheap (a capture only
    // runs when something actually throws), never handles/alters exception dispatch. Safe to call
    // from any thread; called at engine init (AgentCatchLog.h's InstallAgentExceptionTrace).
    void InstallThrowStackCapture() noexcept;

    // "Module.dll+0xRVA" for a code address (module resolved from the address; a non-module
    // address renders as raw hex). Best-effort, never throws.
    std::wstring FormatAddressModuleRva(const void* address) noexcept;

    // Snapshot + format THIS thread's most recent captured throw stacks, newest first, as
    // ready-to-append "[exc]   throw#K (age Nms, F frames): mod+rva ...\n" lines ("" when none).
    // Call BEFORE any classification rethrow (`try { throw; } catch ...`) — the rethrow itself is
    // captured by the VEH and would otherwise displace the entry you came to read.
    std::wstring CaptureRecentThrowStacksText(size_t maxEntries = 2) noexcept;

    // Per-key rate gate for exception logging (a hot-path catch may fire in bursts): true => log
    // now (suppressed = how many were dropped since the last allowed log for this key), false =>
    // drop. ~2s window, thread-safe, best-effort (fails open).
    bool ExcLogThrottleAllow(const std::wstring& key, unsigned& suppressed) noexcept;

    // The one log chokepoint behind every swallowed-exception report: writes
    //   "[exc] <context>: swallowed <detail> tid=0x… (no crash)\n" + the captured throw-stack lines
    // to hooks.log, throttled per context. `detail` = the classified type/hr/message (composed by
    // the caller — the winrt-aware classifier lives in AgentCatchLog.h, above the engine).
    void LogSwallowedExceptionCore(const wchar_t* context, const std::wstring& detail, const std::wstring& stacksText) noexcept;

    // Engine-level "log the current exception" for a plain-C++ catch(...) site (classifies
    // std::system_error / std::exception / unknown — no WinRT here; TerminalApp-side sites use
    // AgentCatchLog.h's AgentLogCaughtException, which adds hresult_error/wil detail).
    // MUST be called from INSIDE a catch block (it rethrows to classify).
    void LogSwallowedException(const wchar_t* context) noexcept;

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

    // The Claude PASTE-CACHE root: <claude-config>/paste-cache (same resolution as ClaudeProjectsDir).
    // Claude Code spills a large paste there AT PASTE TIME (content-addressed <16-hex>.txt leaves), so
    // a pending draft's "[Pasted text #N +M lines]" / "[...Truncated text #N +M lines...]" placeholder
    // has a durable on-disk source the PendingPaste.h resolver can anchor to (PENDING_INPUT.md §2b).
    // Empty if the config dir is unresolvable. [Agentmaster]
    std::wstring ClaudePasteCacheDir();

    // The IMPURE half of the paste resolver (PendingPaste.h is the pure brain): detect the markers in
    // `draft`, read the cache files under `cacheDir` (per-file + total size caps; missing dir => no
    // resolution), run the content-anchored validation, and render ONE compact human/loggable
    // annotation — one line per marker:
    //   "paste #1 (+273 lines) -> bf8eefafa3e80676.txt"
    //   "truncated #2 (+258 lines) -> bf8eefafa3e80676.txt"
    //   "paste #3 (+99 lines) -> unresolved"
    // "" when the draft carries no markers at all. Never throws (I/O failures read as unresolved).
    // The no-suffix overload resolves against ClaudePasteCacheDir(). [Agentmaster]
    std::wstring ResolvePendingPasteRefsIn(const std::wstring& draft, const std::wstring& cacheDir);
    std::wstring ResolvePendingPasteRefs(const std::wstring& draft);

    // ---- Workspace trust (Agentmaster: never let the startup trust dialog wedge a managed tab) ----
    //
    // Claude Code blocks EVERY interactive session on a modal "Accessing workspace: … Is this a
    // project you created or one you trust?" until the workspace is trusted. An unattended ConPTY
    // session can't answer it: no `UserPromptSubmit` ever fires, no transcript id is minted (so the
    // tab shows only the §11d unlinked observe badge), the Tests Autorunner can't drive it, and a
    // /handover content injection is eaten by the menu. Verified against claude 2.1.220 (its own JS
    // gate), the workspace is trusted iff ANY of:
    //   * env CLAUDE_CODE_SANDBOXED is truthy,
    //   * this run already accepted (in-memory only), or the session is a background agent,
    //   * ~/.claude.json projects[<key>].hasTrustDialogAccepted === true, for the cwd's key OR ANY
    //     ANCESTOR directory of the cwd.
    // ⚠ `--dangerously-skip-permissions` / `--permission-mode bypassPermissions` do NOT skip it (the
    // trust gate never consults the permission mode) — an older comment here claimed they did; live
    // PTY-probed false on 2.1.220. Non-interactive `-p` skips it, which is why hooks/print calls
    // never hit this. There is NO settings.json / managed-policy knob (the whole string table was
    // enumerated) — seeding the key below is the remedy Claude Code itself prints.
    //
    // The ~/.claude.json project key for `dir`: its nearest enclosing git root (FindGitRootForDir —
    // a worktree is its own root) else `dir` itself, absolute, with '\' -> '/' and no trailing
    // separator — matching Claude's own key normalization. Keying the REPO (not the exact cwd) is
    // what Claude does natively and means one entry per repo covers every subdirectory. Empty when
    // `dir` is empty/relative. [Agentmaster]
    std::wstring ClaudeWorkspaceTrustKey(std::wstring_view dir);

    // The Claude GLOBAL config file: <CLAUDE_CONFIG_DIR | %USERPROFILE%>\.claude.json. NOTE the base
    // differs from ClaudeProjectsDir's (which falls back to ~/.claude, a DIRECTORY): this file sits
    // beside it at ~/.claude.json. Empty if unresolvable. [Agentmaster]
    std::wstring ClaudeGlobalConfigPath();

    // PURE (unit-tested): given the RAW text of a .claude.json and a trust key, return the text with
    // projects[<key>].hasTrustDialogAccepted set to true — or nullopt when NO write is needed/safe:
    //   * the key is already trusted (the steady state after the first launch — so a spawn costs one
    //     read and zero writes),
    //   * the text is empty / not parseable / not a JSON object (never rebuild a config we can't
    //     read — the Updater.h "refuse to clobber" rule; this file holds the user's oauth account),
    //   * the splice would produce text that no longer parses, or that doesn't read back as trusted.
    // It is a SURGICAL SPLICE, never a re-serialize: every byte outside the inserted run is
    // preserved verbatim. That is deliberate — Json.h prints numbers through "%g" (6 significant
    // digits), so round-tripping the user's real 130 KB config would silently truncate float fields
    // (lastCost, lastFpsAverage, …). The insertion copies the surrounding indentation so the
    // pretty-printed file stays pretty. [Agentmaster]
    std::optional<std::wstring> SpliceWorkspaceTrust(const std::wstring& configText, const std::wstring& key);

    // Pre-trust the workspace containing `dir` by seeding its ~/.claude.json key, so the next claude
    // launched there never shows the trust dialog. Best-effort + no-throw: returns true only when the
    // workspace is trusted on return (already-trusted counts). Writes ONLY when the flag is missing
    // or false — i.e. once per workspace, ever.
    //
    // Interlocks with Claude's OWN config writer: it guards ~/.claude.json with a proper-lockfile
    // lock at "<path>.lock", whose primitive is an atomic mkdir — so we take the same lock the same
    // way (CreateDirectoryW), briefly, and skip the seed entirely if Claude holds it (fail-open: the
    // dialog shows once more and the next launch retries). A lock left behind by a killed process is
    // broken only once it is far past proper-lockfile's own 10 s staleness horizon. Contention waits
    // are bounded at 12 x 25 ms, and only the first launch in a workspace can wait at all — the
    // already-trusted path never takes the lock — so the launch seam adds one ~1 ms read in steady
    // state and at most ~300 ms once per workspace.
    //
    // This is a deliberate, additive mutation of ~/.claude.json — the second write outside the
    // profile (after the /handover command definitions), gated on AppSettings::trustWorkspaceOnLaunch
    // (default ON). It only ever sets a flag to true; nothing is removed or rewritten. [Agentmaster]
    bool EnsureClaudeWorkspaceTrusted(std::wstring_view dir);

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

    // Agentmaster (native-exe-only policy — the "Claude not detected" install prompt). Detect a LEGACY
    // npm/Node claude LAUNCHER on PATH: the first `claude.cmd`/`claude.bat` found, returned as a FULL
    // path (empty if none). This is the case that STILL shows the not-found modal even though "npm claude
    // is installed" — a .cmd/.bat whose FollowNpmCmdToExe found no native binary behind it (the pure-Node
    // CLI); the modal offers `<that launcher> install` (the npm->native migration) for it. Deliberately
    // the INVERSE of ResolveClaudeExe (which only ever returns a native .exe): here we WANT the .cmd/.bat,
    // by full path, so `claude install` runs the REAL npm launcher and bypasses our --settings shim.
    // `excludeDir` (normalized, trailing-slash) is skipped case-insensitively — the caller passes OUR
    // <profile>\shim dir so a legacy claude isn't falsely detected as our own prepended shim claude.cmd.
    // `...In` is the OS-light, unit-testable core (PATH dirs in); the wrapper gathers PATH + the shim dir
    // from the environment.
    std::wstring NpmClaudeLauncherInPath(const std::vector<std::wstring>& pathDirs, std::wstring_view excludeDir = {});
    std::wstring NpmClaudeLauncherOnPath();

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

    // Agentmaster (COMMANDS.md — the /handover integration). Ensure the `/handover` slash-command
    // DEFINITION exists at <configDir>\commands\handover.md — the file that makes a typed
    // `/handover <context-or-filepath>` a real Claude Code command (an unknown command is rejected
    // client-side and never reaches the transcript). Its body instructs Claude to write ONE
    // handover markdown via the Write tool — exactly the signal the CommandWatch's markdown await
    // keys on — and (since the content-injection change) to write it AS the successor's first user
    // message, because the file's content is injected verbatim. Write policy: create-if-absent,
    // PLUS a VERSION-AWARE UPGRADE — a file whose SHA-256 matches a PRIOR shipped version
    // (ShippedHandoverCommandHashes) is ours, untouched by the user, and is silently upgraded to
    // the current text; anything else — the user's own /handover, or an edited copy of ours — is
    // NEVER overwritten (the ApplyEnvDefaults discipline: a user edit sticks forever). Still the
    // deliberate, additive write into the user's GLOBAL ~/.claude config, inert until the command
    // is typed; the one place Agentmaster writes outside its profile (COMMANDS.md §6). Returns the
    // file's full path ("" on I/O failure), whether created, upgraded, or already present.
    // `...In` takes the Claude CONFIG dir explicitly (the unit-testable core; the harness points it
    // at a temp dir so tests never touch the real ~/.claude); the wrapper resolves
    // CLAUDE_CONFIG_DIR > ~/.claude, exactly like ClaudeProjectsDir.
    std::wstring EnsureHandoverCommandFileIn(const std::wstring& configDir);
    std::wstring EnsureHandoverCommandFile();

    // The ordered shipped-version history of the /handover command definition, as the SHA-256
    // (lowercase hex) of each version's UTF-8 bytes exactly as they are written to disk — oldest
    // first, the LAST entry being the digest of the CURRENT text (ShippedHandoverCommandText, what
    // EnsureHandoverCommandFile writes). Exposed for the upgrade rule + its tests: an on-disk file
    // whose digest matches any PRIOR entry is a pristine older OURS and upgrades to the current
    // text; anything else is user-owned and untouched. DIGESTS, not the texts: recognizing "a
    // version we shipped, unmodified" is a content-IDENTITY question, so a superseded version costs
    // one 64-char line instead of a frozen multi-KB literal (the texts stay in git history). The
    // list is APPEND-ONLY — editing or dropping an entry orphans every install still on that
    // version (it would read as user-owned and never upgrade again).
    const std::vector<std::string_view>& ShippedHandoverCommandHashes();

    // The CURRENT /handover definition text (the last version of the history above; its digest is
    // ShippedHandoverCommandHashes().back(), an identity the engine harness asserts so a text edit
    // without an appended digest fails the suite).
    std::wstring_view ShippedHandoverCommandText();

    // Agentmaster (COMMANDS.md — the /handover-here integration): the IN-PLACE twin of /handover.
    // Its OWN definition file at <configDir>\commands\handover-here.md under the SAME write policy
    // (create-if-absent + the version-aware upgrade; a user edit sticks forever) and the SAME
    // await-signal contract ("use the Write tool", the `HANDOVER-<topic>.md` leaf) — but the
    // briefed outcome differs: instead of opening a successor TAB beside the origin, Agentmaster
    // RESTARTS the origin tab in place into a fresh "New Session Here -> Default" conversation
    // (the origin is archived, resumable from the Sessions browser) and injects the document as
    // its first user message. Same wrappers/returns as the /handover pair above.
    std::wstring EnsureHandoverHereCommandFileIn(const std::wstring& configDir);
    std::wstring EnsureHandoverHereCommandFile();

    // The /handover-here definition's shipped-version history + current text — the
    // ShippedHandoverCommandHashes contract verbatim: SHA-256 per version, oldest first, the LAST
    // entry being the current text's digest, entries frozen forever, only ever APPEND.
    const std::vector<std::string_view>& ShippedHandoverHereCommandHashes();
    std::wstring_view ShippedHandoverHereCommandText();

    // Agentmaster (COMMANDS.md §5b — the /handover-standby integration): the FILL-NOT-SEND member
    // of the family. Its OWN definition file at <configDir>\commands\handover-standby.md under the
    // SAME write policy and the SAME await-signal contract ("use the Write tool", the
    // `HANDOVER-<topic>.md` leaf) — but the briefed outcome is a handover IN STANDBY: each file's
    // successor opens as a new tab like /handover, and the document is TYPED into that session's
    // input box WITHOUT being submitted (BuildPromptFill — the bracketed paste minus the submit
    // CR), so the briefing sits exactly one Enter away and NOTHING runs until the user sends it.
    // Same wrappers/returns as the /handover pair above.
    std::wstring EnsureHandoverStandbyCommandFileIn(const std::wstring& configDir);
    std::wstring EnsureHandoverStandbyCommandFile();

    // The /handover-standby definition's shipped-version history + current text — the same frozen
    // append-only digest contract as its two siblings.
    const std::vector<std::string_view>& ShippedHandoverStandbyCommandHashes();
    std::wstring_view ShippedHandoverStandbyCommandText();

    // The shared core BOTH shipped-definition writers route through, so the write policy can never
    // drift between the two files (COMMANDS.md §6): create-if-absent PLUS the version-aware upgrade
    // — an existing file whose SHA-256 matches a PRIOR entry of `shippedHashes` (the command's full
    // history, oldest first, CURRENT text's digest last) is rewritten with `currentText`; anything
    // else is left exactly as it is. Returns the definition's full path ("" only on a failed
    // create). Exposed for the harness: the real histories carry digests only, so a prior version's
    // bytes no longer exist to lay on disk — the upgrade/leave-alone policy is unit-tested over a
    // SYNTHETIC command whose own "prior version" text the test still holds.
    std::wstring EnsureShippedCommandFileIn(const std::wstring& configDir,
                                            std::wstring_view fileLeaf,
                                            const std::vector<std::string_view>& shippedHashes,
                                            std::wstring_view currentText,
                                            std::wstring_view logLabel);

    // ---- CUSTOMIZABLE COMMAND NAMES (COMMANDS.md §6a — the cog's "Commands" tab) ----
    // A /handover-family command can be RENAMED: its definition then lives at <configDir>\commands\
    // <name>.md and its text mentions "/<name>" instead of "/<defaultName>" (the self-invocation
    // guard tells the user what to TYPE, so the token must match the file's actual name). The
    // shipped-version histories stay DIGESTS OF THE DEFAULT-NAME TEXTS — so identity questions
    // about a custom-named file first normalize its bytes BACK to the default-name form and then
    // hash. That one trick makes every shipped version recognizable under ANY name without ever
    // freezing per-name digests: render(text, name) and the byte normalization are exact inverses
    // (both substitute the "/<word>" token at a word boundary only; names are ASCII slugs —
    // NormalizeCommandName — so the UTF-8 byte substitution can never split a multi-byte char).

    // Render a shipped command text for a custom name: every "/<defaultName>" token (word-boundary
    // bounded, so a "/handover" can never corrupt a "/handover-here" mention) becomes
    // "/<commandName>". commandName == defaultName returns the text verbatim.
    std::wstring RenderShippedCommandText(std::wstring_view text, std::wstring_view defaultName, std::wstring_view commandName);

    // The identity inverse over a file's raw UTF-8 bytes: every "/<commandName>" token becomes
    // "/<defaultName>", so the result can be SHA-256'd against the shipped history. commandName ==
    // defaultName returns the bytes verbatim.
    std::string NormalizeCommandBytesForIdentity(std::string_view bytes, std::wstring_view defaultName, std::wstring_view commandName);

    // The name-aware EnsureShippedCommandFileIn: materialize <commandName>.md carrying
    // RenderShippedCommandText(currentText, defaultName, commandName), under the SAME write policy
    // (create-if-absent + the version-aware upgrade, digest identity through the byte
    // normalization above; a user-edited file is NEVER overwritten). Returns the file's full path
    // ("" on failure / degenerate input).
    std::wstring EnsureShippedCommandFileNamedIn(const std::wstring& configDir,
                                                 std::wstring_view defaultName,
                                                 const std::vector<std::string_view>& shippedHashes,
                                                 std::wstring_view currentText,
                                                 std::wstring_view logLabel,
                                                 std::wstring_view commandName,
                                                 std::wstring_view writePath = {});

    // ---- CUSTOMIZABLE WRITE LOCATION (COMMANDS.md §6c — the cog's "Handover file location") ----
    // WHERE the family tells Claude to write its HANDOVER-<topic>.md briefings. FOLDER-ONLY: the
    // file NAME contract is untouched (the file-match regex, the one-successor-per-file fan-out and
    // delete-after all key on it) — only the directory moves. The shipped texts carry ONE
    // "WRITE IT IN: <phrase>" line whose phrase is the shipped default (the session scratchpad);
    // materializing renders the configured location into it, and identity questions fold whatever
    // is there BACK to the shipped phrase before hashing. Unlike the name pair, the inverse is
    // DELIMITED (marker .. end of line), so it needs no per-install marker and still recognizes a
    // location someone changed by hand. KEEP THE MARKER + A ONE-LINE PHRASE in every future text.

    // The one-line phrase a configured write path renders to: the shipped scratchpad sentence for
    // "" / "scratchpad", "the current working directory" for "./", else the value backtick-quoted
    // with a create-if-missing note (absolute vs relative-to-the-working-directory).
    std::wstring CommandWritePathPhrase(std::wstring_view writePath);

    // Render a shipped command text for a configured write path (a no-op for the shipped default).
    std::wstring RenderShippedCommandWritePath(std::wstring_view text, std::wstring_view writePath);

    // The identity inverse over a file's raw UTF-8 bytes: every "WRITE IT IN: …" line's phrase
    // becomes the shipped one, so the result can be SHA-256'd against the shipped history. A text
    // without the marker (every pre-§6c version) is returned verbatim — which is exactly what keeps
    // those versions matching their own historical digests.
    std::string NormalizeCommandWritePathBytesForIdentity(std::string_view bytes);

    // What a definition file on disk IS, relative to what Agentmaster would write now — the cog's
    // honesty line + the gate for its Reinstall button. (The enum lives in SessionModels.h so the
    // UI headers can hold one without pulling in this header's spawn machinery.)
    ShippedCommandFileState InspectShippedCommandFileNamedIn(const std::wstring& configDir,
                                                            std::wstring_view defaultName,
                                                            const std::vector<std::string_view>& shippedHashes,
                                                            std::wstring_view currentText,
                                                            std::wstring_view commandName,
                                                            std::wstring_view writePath = {});

    // The user-initiated, confirmed OVERWRITE of a definition file, digest regardless — the escape
    // hatch for a hand-edited definition, which the normal policy freezes forever (and which would
    // therefore keep pointing at the old write location). Returns the path ("" on failure).
    std::wstring ForceReinstallShippedCommandFileNamedIn(const std::wstring& configDir,
                                                         std::wstring_view defaultName,
                                                         std::wstring_view currentText,
                                                         std::wstring_view logLabel,
                                                         std::wstring_view commandName,
                                                         std::wstring_view writePath = {});

    // Remove <configDir>\commands\<commandName>.md IFF its (name-normalized) digest matches ANY
    // shipped version of this command — i.e. it is OURS and untouched, under whatever name. The
    // rename/disable migration: the old file goes away so Claude stops offering a dead command.
    // A user-edited (or foreign same-named) file never matches and is LEFT IN PLACE; returns
    // whether the file was actually deleted (logged "[engine] <label> command removed …").
    bool RemoveShippedCommandFileNamedIn(const std::wstring& configDir,
                                         std::wstring_view defaultName,
                                         const std::vector<std::string_view>& shippedHashes,
                                         std::wstring_view logLabel,
                                         std::wstring_view commandName);

    // The per-command engine-init reconcile (COMMANDS.md §6a): when the previously-materialized
    // name (the durable AppSettings marker; "" == nothing materialized) differs from what is now
    // wanted (configuredName, or nothing when !enabled), migrate the old file away (the ours-only
    // Remove above), then materialize the configured name (the named Ensure). Returns the NEW
    // marker value — configuredName on success, "" when disabled or the write failed (a failed
    // write self-heals: next init sees marker "" and just materializes again).
    std::wstring ReconcileShippedCommandFileIn(const std::wstring& configDir,
                                               std::wstring_view defaultName,
                                               const std::vector<std::string_view>& shippedHashes,
                                               std::wstring_view currentText,
                                               std::wstring_view logLabel,
                                               std::wstring_view previouslyMaterializedName,
                                               std::wstring_view configuredName,
                                               bool enabled,
                                               std::wstring_view writePath = {});

    // The per-command values every family-level entry below reports, in ONE named shape (the
    // family grew to three commands, so the old std::pair returns became these — structured
    // bindings still read them positionally: `const auto [ho, hh, hs] = …`).
    struct HandoverFamilyNames
    {
        std::wstring handover;
        std::wstring here;
        std::wstring standby;
    };
    struct HandoverFamilyFileStates
    {
        ShippedCommandFileState handover{ ShippedCommandFileState::Missing };
        ShippedCommandFileState here{ ShippedCommandFileState::Missing };
        ShippedCommandFileState standby{ ShippedCommandFileState::Missing };
    };

    // The engine-init entry: reconcile ALL /handover-family definition files against the
    // settings' configured names/enables (each command through ReconcileShippedCommandFileIn).
    // Returns the materialized names { handover, here, standby } — the values the engine RMWs
    // back into the AppSettings markers. The `In` form takes the Claude config dir explicitly
    // (the unit-testable core); the wrapper resolves CLAUDE_CONFIG_DIR > ~/.claude like
    // EnsureHandoverCommandFile.
    HandoverFamilyNames ReconcileHandoverCommandFilesIn(const std::wstring& configDir, const AppSettings& settings);
    HandoverFamilyNames ReconcileHandoverCommandFiles(const AppSettings& settings);

    // COMMANDS.md §6c — the three family-level entries the Settings cog drives, all keyed on the
    // LIVE (materialized) command names so they act on the definition files actually in use, and
    // none of them rename/migrate anything (a rename stays restart-applied — §6a):
    //  * Refresh   — re-render the live definitions with the CURRENT write location (the cog's Save
    //                calls it, which is what makes the location apply with no restart). Honors the
    //                normal write policy: a user-edited definition is left frozen.
    //  * Reinstall — the confirmed OVERWRITE of the files, digest regardless (the escape hatch).
    //  * Inspect   — what each file currently IS (the cog's status line + the Reinstall nudge).
    // Each returns per-command values in { handover, here, standby } order; a command with no
    // definition to act on (disabled, nothing materialized) yields "" / Missing.
    HandoverFamilyNames RefreshHandoverCommandWritePathIn(const std::wstring& configDir, const AppSettings& settings);
    HandoverFamilyNames RefreshHandoverCommandWritePath(const AppSettings& settings);
    HandoverFamilyNames ReinstallHandoverCommandFilesIn(const std::wstring& configDir, const AppSettings& settings);
    HandoverFamilyNames ReinstallHandoverCommandFiles(const AppSettings& settings);
    HandoverFamilyFileStates InspectHandoverCommandFilesIn(const std::wstring& configDir, const AppSettings& settings);
    HandoverFamilyFileStates InspectHandoverCommandFiles(const AppSettings& settings);

    // COMMANDS.md §6b (successor shaping — the TITLE rewrite): the successor-title CANDIDATE under
    // the user's regex find/replace pair, or "" when the rewrite does not apply — findRegex unset,
    // invalid (RegexUtil.h), matching nowhere in the origin title, or producing a
    // whitespace-only result (a title must never go empty/blank — Rule #11). On "" the caller
    // falls back to the classic DeriveSuffixedTitle "(handover)" naming, so the rewrite can only
    // ever IMPROVE a title, never lose one. $1-style backrefs honored, every occurrence replaced
    // (RegexReplace); the result is trimmed and, degenerate-guarded like DeriveSessionTitle,
    // capped at 255 chars (252 + "..."). PURE + unit-tested; the caller still uniqueness-bumps
    // the candidate past registry titles exactly like the default naming.
    std::wstring DeriveHandoverSuccessorTitle(std::wstring_view originTitle, std::wstring_view findRegex, std::wstring_view replacement);

    // COMMANDS.md §6b (successor shaping — the per-MESSAGE hints): a /handover(-here/-standby)
    // message may LEAD with a model pick and/or an explicit bracketed successor TITLE, each
    // overriding its configured counterpart (the "Successor model" combo / the title rewrite +
    // classic "(handover)" naming) for THAT handover alone:
    //   `/handover [fable] do a b c`               (model)
    //   `/handover fable 5: fix the tests`         (model, bare form)
    //   `/handover [fable] [my title] do a b c`    (model + title)
    //   `/handover-standby [my title] fix x`       (title alone — the first bracket matched no
    //                                               model, so it FELL BACK to the title slot)
    // Given the command's ARGS (the transcript echo's <command-args>, delivered on the fire) and
    // the launchModels spec, returns both hints ("" == that hint absent — the configured behavior
    // applies).
    //
    // MODEL matching is PARTIAL + CASELESS + CHARACTERS-ONLY: both the typed hint and each entry's
    // TWO sides (the display name AND the model id, ParseLaunchModels) fold to lowercase [a-z0-9]
    // (spaces/dots/hyphens/brackets dropped), and the hint matches an entry when it is a SUBSTRING
    // of either folded side — "fable" hits the shipped "Fable"/"fable" and equally a user's pinned
    // "Fable 5"/"claude-fable-5"; "sonnet" hits "claude-sonnet-5". First matching entry in list
    // order wins. Only the args' FIRST LINE's
    // LEADING portion is consulted:
    //   * `[hint]` — the explicit bracketed form: the bracket body is the hint (any non-empty fold;
    //     an absurdly long bracket or a no-match is not a model — it FALLS BACK to the TITLE slot
    //     below; an UNTERMINATED first bracket reads as no hints at all — plain context);
    //   * bare leading word(s) — the FIRST word must hit on its own (and fold to >= 3 chars — a
    //     stray "a"/"do" can never accidentally pick a model), then the hint greedily EXTENDS one
    //     word at a time (up to 4) while the longer fold still matches, longest match winning —
    //     so "fable 5 do x" resolves "fable5" (not just "fable") and stops before "do".
    // TITLE is BRACKETED ONLY, in one of two positions: the FIRST bracket when its body matches no
    // model (or none are configured — the fallback semantics), or the NEXT token right after a
    // recognized model hint (bracketed or bare — "fable 5 [my title] fix x" works too). The body
    // is taken VERBATIM (any characters — "[my asd \n !!_ title]" keeps its punctuation and its
    // LITERAL backslash-n exactly as typed), edge-trimmed, "" when blank, degenerate-capped at 255
    // (252 + "...") like every title path. Deeper brackets are never consulted (a "[x]"
    // mid-sentence stays context). The caller gives the explicit title precedence over the §6b
    // find/replace rewrite AND the classic "(handover)" naming; its registry uniqueness bump still
    // applies (a fan-out's later files walk "<title> (handover)" / "(handover 2)" …).
    // The hints are NOT stripped from anything: the origin already received the full text as
    // $ARGUMENTS (harmless context), and the successor's first message is the FILE content. PURE
    // (ParseLaunchModels over the spec, no I/O) + unit-tested.
    struct HandoverArgsHints
    {
        std::wstring modelId; // "" == no model hint (the per-command combo / Default applies)
        std::wstring title; // "" == no explicit title (the rewrite / "(handover)" naming applies)
    };
    HandoverArgsHints ParseHandoverArgsHints(std::wstring_view args, std::wstring_view launchModelsSpec);
    // The model-only view of the same parse (the §6b original signature) — delegates to
    // ParseHandoverArgsHints, ONE parser, so the two can never disagree.
    std::wstring PickModelFromArgsHint(std::wstring_view args, std::wstring_view launchModelsSpec);

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
    // `modelOverride` (Agentmaster, launch-model picker): non-empty => the commandline carries
    // ` --model <id>` — the per-LAUNCH model picked from an "Open New Session Here" submenu (see
    // BuildClaudeCommandline). Per-launch only, deliberately NOT persisted on the session: a later
    // resume/restart follows the settings model again (the pick was for THAT launch).
    // `initialPrompt` (Agentmaster, COMMANDS.md — the /handover successor): non-empty => the
    // commandline carries it as the trailing positional prompt claude submits as the session's
    // FIRST turn (see BuildClaudeCommandline). Per-launch only, never persisted: a restart/resume
    // must not re-submit it (the turn already ran and lives in the transcript).
    ClaudeSpawnSpec BuildClaudeSpawn(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view resumeSessionId, const AppSettings& settings, std::wstring_view forkFromSessionId = {}, std::wstring_view claudeLauncher = {}, std::wstring_view forkIntoSessionId = {}, std::wstring_view modelOverride = {}, std::wstring_view initialPrompt = {});

    // Build a spec to RELAUNCH an existing managed conversation IN PLACE — the tab's connection died and
    // the user hit "Restart session" (WT's restartConnection). Unlike BuildClaudeSpawn it NEVER mints a
    // new id: it KEEPS <sessionId> and picks the relaunch form from the CURRENT on-disk state —
    //   * a transcript exists                      -> RESUME it (claude --resume <id>);
    //   * none, but `forkParentId` still has one   -> RE-FORK from the parent INTO the same id
    //     (claude --resume <parent> --fork-session --session-id <id> — the _LaunchClaudeSession
    //     [restore->refork] recipe: a NEVER-MESSAGED fork writes no transcript until its first turn, so
    //     a plain fresh relaunch would silently replace the forked branch with an EMPTY conversation);
    //   * neither                                  -> FRESH reusing <sessionId> (claude --session-id <id>
    //     — collision-free since the id is unused on disk, keeps the registry/tab/injector binding stable).
    // It must be used INSTEAD of replaying the original launch commandline, which is `--session-id
    // <id>` for a fresh launch (collides with the now-existing transcript) or a `--fork-session` form for a
    // fork (would re-fork a since-grown id). `claudeLauncher` is the resolved full path (BuildClaudeSpawn
    // semantics); `forkParentId` is the session's persisted fork SOURCE (SessionInfo::forkParentId; empty
    // for a non-fork or once the fork produced content).
    ClaudeSpawnSpec BuildClaudeRestartSpec(std::wstring_view workingDir, std::wstring_view title, std::wstring_view pipeName, std::wstring_view sessionId, const AppSettings& settings, std::wstring_view claudeLauncher = {}, std::wstring_view forkParentId = {});
}
