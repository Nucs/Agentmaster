# Agentmaster — Environment variables passed to Claude / Codex sessions

> **Status: IMPLEMENTED (engine + persistence + cog UI; pending a compile/deploy cycle).** The
> Settings cog's old single `;`-delimited "Environment variables" box is replaced by a two-tab
> **Global / Per-directory** area: a multi-line `NAME=VALUE` editor (one per line) with a **live
> lexer** that recolors the editor's border + writes a bottom status line, and a typable/filtered
> directory selector for the per-dir tab. One shared set feeds **both** Claude and Codex. The
> per-directory overrides live in a new `dir-env.json` (keyed like `dir-colors.json`); the global
> set stays `AppSettings.env`. They merge at spawn (per-dir wins) via `ResolveSessionEnv`.
> Companions: [`PROFILES.md`](./PROFILES.md) (where state lives) · [`OBSERVER.md`](./OBSERVER.md)
> (`AM_SESSION` stamp) · [`HOOKS.md`](./HOOKS.md) (`CCMGR_*`) · [`DESIGN.md`](./DESIGN.md).

---

## 1. How env vars reach claude/codex — the mechanism

A session is a real `claude.exe` / `codex.exe` on a **ConPTY** (hosted under an interactive `pwsh`).
We don't fight Windows env inheritance — **we ride it**:

1. The child process is created by `ConptyConnection` via `CreateProcessW` with a custom env block.
   Because we pass `reloadEnvironmentVariables = false` and an empty `initialEnvironment`, the **base
   block is Agentmaster's own current process environment** (`til::env::from_current_environment()`,
   `ConptyConnection.cpp`).
2. Our **per-spawn overrides** are layered on top (`environment.set_user_environment_var(...)`), so a
   variable we set **wins** over an inherited same-named one. WT also injects `WT_SESSION` /
   `WT_PROFILE_ID`; we add `AM_SESSION` (ownership stamp) and, for Claude, `CCMGR_SESSION_ID` /
   `CCMGR_HOOK_PIPE` (hook correlation).
3. The direct child is **pwsh**; `claude`/`codex` run as its **descendants** and inherit the block by
   normal process-tree inheritance. So the merged env reaches the agent unchanged.

This is the *launched* path — it is reliable (unlike a hand-typed `+`-tab claude, whose env WT
regenerates; see the Gotchas in `CLAUDE.md`). The override map is built in
`TerminalPage::_BuildAgentConnection` from `spec.env` (Claude) / the resolved list (Codex):
`CreateSettings(cmd, dir, title, /*reload*/ false, /*initialEnv*/ L"", envMap, …)`.

**So "how do we pass env vars when claude/codex run from our spawning process?"** — they inherit our
process block, and ConPTY lets us layer per-spawn overrides on top of it. No registry trick, no shim.

## 2. The two layers + precedence

| Layer | Stored in | Format | Keyed by |
|---|---|---|---|
| **Global** | `AppSettings.env` (`settings.json`) | newline-delimited `NAME=VALUE` (legacy `;` still parses) | — |
| **Per-directory** | `dir-env.json` (profile dir) | map → newline-delimited `NAME=VALUE` | `NormDirKey(workingDir)` |

Effective env for a session spawned in `dir`, most-specific wins:

```
Agentmaster process block  →  Global (AppSettings.env)  →  Per-dir (dir-env.json[NormDirKey(dir)])  →  CCMGR_*/AM_SESSION (ours)
```

- Per-dir entries **override** a global entry of the same name (Windows env names are
  case-**insensitive**, so the match is too). Within one block, the **last** assignment of a name wins.
- `CCMGR_*` is **dropped** from user input (reserved for hook correlation — the spawn injects it after,
  so it can never be clobbered). `AM_SESSION` / `WT_SESSION` are set by the connection; a user value is
  ignored (the lexer warns).
- One set, **both agents** — Claude and Codex read the same merge (proxies/keys usually apply to both).

## 3. Engine API (pure + unit-tested) — `ClaudeSpawn.{h,cpp}`

```cpp
// Parse a NAME=VALUE list. Separators: newline (the editor) OR ';' (legacy). '#' line = comment.
std::vector<std::pair<std::wstring,std::wstring>> ParseEnvAssignments(std::wstring_view spec);

// PURE merge: per-dir overrides global (case-insensitive), CCMGR_* dropped, last-wins, first-seen order.
std::vector<std::pair<std::wstring,std::wstring>> MergeSessionEnv(std::wstring_view global, std::wstring_view perDir);

// MergeSessionEnv(settings.env, GetDirEnv(workingDir)) — the env applied to a session in `workingDir`.
std::vector<std::pair<std::wstring,std::wstring>> ResolveSessionEnv(const AppSettings&, std::wstring_view workingDir);

// Per-line lexer for the cog's live border + status. Ok / Warn (reserved/owned/duplicate) / Error
// (no '=', empty/invalid name) / Ignored (blank/comment), plus worst level + first issue.
EnvLexResult LexEnvText(std::wstring_view text);   // EnvLineKind / EnvLineDiag / EnvLexResult
```

Spawn sites all route through `ResolveSessionEnv` (one helper, no duplicated CCMGR-filter loops):
- **Claude** — `AppendManagedClaudeEnv` (shared by launch / resume / fork / in-place restart).
- **Codex** — `_LaunchCodexSession` + the codex branch of `_RestartManagedSession`.

## 4. Persistence — `dir-env.json` (`Persistence.{h,cpp}`)

Mirrors `dir-colors.json` exactly (same `NormDirKey` keying + thread-safe load-modify-save):

```cpp
std::wstring SerializeDirEnv(const std::vector<std::pair<std::wstring,std::wstring>>&);
std::vector<std::pair<std::wstring,std::wstring>> DeserializeDirEnv(std::wstring_view);
void SaveDirEnv(const std::vector<std::pair<std::wstring,std::wstring>>&);
std::vector<std::pair<std::wstring,std::wstring>> LoadDirEnv();
std::wstring GetDirEnv(const std::wstring& dir);            // "" if none
void SetDirEnv(const std::wstring& dir, std::wstring_view); // blank => erase the entry
```

`{"version":1,"dirs":[{"dir":"<NormDirKey>","env":"NAME=VALUE\nNAME=VALUE"}]}`. A blank-env entry is
dropped on load (a cleared editor removes the dir). The cog is the only writer; engine code stays
profile-resolved through `AgentmasterStateDir()` (Rule #15).

## 5. The cog UI — `AgentManagerContent` (`_BuildEnvVarsArea` & friends)

A self-contained area in the Settings overlay (in-content modal, so `TextBox` typing works — *not* a
`ContentDialog`):

```
ENVIRONMENT VARIABLES        [ Global ][ Per-directory ]
─────────────────────────────────────────────────────────
GLOBAL ▸  ┌───────────────────────────────────┐  ← border = lexer status
          │ HTTPS_PROXY=http://h:8080          │     (gray rest / green ok / amber warn / red error)
          │ # one NAME=VALUE per line          │
          └───────────────────────────────────┘
          ✓ 1 variable                            ← status line

PER-DIR ▸ [ type to filter directories…        ]  ← filters the list below
          ┌ ● c:\work\proj                      ┐  ← session dirs ∪ dirs with env (● = has env)
          │   c:\work\other                     │
          └─────────────────────────────────────┘
          Variables for: c:\work\proj
          ┌───────────────────────────────────┐  ← border = lexer status
          │ AWS_PROFILE=dev                     │
          └───────────────────────────────────┘
          ✓ 1 variable
```

- **Tabs** — two `Button`s swap two panels (the LOCAL/GLOBAL scope-toggle idiom; not a `Pivot`).
- **Editors** — multi-line monospace `TextBox` (`AcceptsReturn`), own border suppressed, wrapped in a
  `Border` recolored by `LexEnvText` (the same green/amber/red palette as `_ValidateLaunchBox`); a
  status `TextBlock` beneath shows `✓ N variables` / `⚠ … (line M)` / `✕ … (line M)`.
- **Per-dir selector** — a filter `TextBox` + a `ListBox` of `session dirs ∪ dir-env keys` (● marks
  dirs that already have env; sorted env-first then alpha); selecting a row loads that dir's env.
- **Draft semantics** — per-dir edits accumulate in an **in-memory draft** (`_dirEnvDraft`) seeded from
  `dir-env.json` on open and flushed to `SaveDirEnv` on **Save**, so **Cancel discards** them (matching
  the cog's form semantics for `AppSettings`). Global writes `AppSettings.env` on Save via the existing
  `_settingsSink`. A legacy `;`-string is shown one-per-line on load and saved back newline-delimited.

## 6. Tests (`tests/m5_tests.cpp`)

`ParseEnvAssignments` (newline / CRLF / mixed `;`+newline / `#` comments), `MergeSessionEnv`
(per-dir override, case-insensitive, CCMGR_* drop, last-wins, empty), `LexEnvText` (ok / 3-error /
3-warn / ignored, worst + firstIssueLine), and a `SerializeDirEnv`/`DeserializeDirEnv` round-trip
(multi-line survives, blank dropped).

## 7. Not done / deferred

- **Per-session** env (a third, most-specific layer keyed by conversation id) — deferred; the
  directory layer covers the common "this folder needs a proxy/key" case (decided in the design Q&A).
- **Effective-env preview** (show the merged result for a dir) — the status line notes counts only;
  a full preview is a possible later polish.
- **Codex `BuildCodexCommandline`** still emits a bare `codex` token (a separate latent PATHEXT bug,
  unrelated to env) — tracked in `CLAUDE.md` Gotchas, not here.
