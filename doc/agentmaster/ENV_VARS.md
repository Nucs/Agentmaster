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

## 8. Shipped global defaults (seeded once; user-removable)

Agentmaster ships two **defaults every install gets** — *new installs AND updaters alike* — that the
user is then free to **edit or delete** (a deleted default never silently returns). Both are
**marker-gated** one-time seeds run at engine init (`Engine.cpp`, right after the dir-colors seed),
before any window opens the cog or a session spawns.

| Default | Where it lands | Default value | Why |
|---|---|---|---|
| `CLAUDE_CODE_MAX_RETRIES` | the **Global env** (`AppSettings.env`) — editable in the cog's *Environment variables ▸ Global* tab | `50000` | Maximize API-retry resilience for unattended Tests Autorunner runs. ⚠️ Claude **clamps the effective value to 15** since CLI **v2.1.186** (`CLAUDE_CODE_RETRY_WATCHDOG` is the newer unattended path); the literal `50000` is intentional + harmless, and the user may change it. |
| `cleanupPeriodDays` | the user's **global `~/.claude/settings.json`** (NOT env, NOT `AppSettings`) — editable in the cog's *Claude history ▸ Keep Claude history (days)* field | `36500` (~100y) | So Claude **never purges global session history** at startup (Resume + the Sessions browser depend on it). The purge is global — any externally-launched `claude` on the default 30 would otherwise delete the whole `~/.claude/projects` store — so we write the **user's own** settings file, not just our `--settings`. |

**Footguns we deliberately avoid** (verified against the official docs + issue tracker):
- `cleanupPeriodDays: 0` does **not** mean "keep forever" — in Claude it **disables transcript
  persistence entirely** (you lose everything). We ship `36500`, never `0`.
- `CLAUDE_CODE_SKIP_PROMPT_HISTORY=1` likewise **disables** persistence (since v2.1.77). We never set it
  (and the env lexer would flag it as just another var — it's the user's call, but the default omits it).

### Seeding mechanism — "everyone gets it once, deletion sticks"

Two markers on `AppSettings` (persisted in our `settings.json`, never shown in the cog) record that the
one-time seed ran, so the seed is **idempotent** and **respects user removal**:

- **`envDefaultsVersion`** (`uint32`, default `0`). `ApplyEnvDefaults(envText, seededVersion)` (pure,
  `ClaudeSpawn.{h,cpp}`) appends each shipped default whose `introVersion > seededVersion` and whose NAME
  isn't already present (case-insensitive), then returns the new version. `SeedSessionEnvDefaults`
  (`Persistence.cpp`) does Load → apply → Save. A pre-feature `settings.json` reads `0` ⇒ seeds once ⇒
  bumps to `kEnvDefaultsVersion` (1). Adding a future default = bump the constant + give it the new
  `introVersion`, and only *it* seeds on the next launch. A default the user deletes never returns
  (its version is already ≤ the stored marker).
- **`claudeCleanupDaysSeeded`** (`bool`, default `false`). `SeedClaudeCleanupPeriodDaysIfNeeded` writes
  `cleanupPeriodDays=36500` **only when the user hasn't set it themselves** (`GetClaudeCleanupPeriodDays`
  is empty), then sets the marker — so it's never re-seeded after the user changes or removes it.

The cog's `_SaveSettings` **preserves both markers from disk** (they're written out-of-cog by the
engine seed), exactly like the summary-panel / updater fields — so a Save can never reset a marker and
re-add a deleted default.

### The Claude user-settings repository (`Persistence.{h,cpp}`)

`cleanupPeriodDays` is the user's, not ours, so a small **managed layer** owns the read/write of their
global file and never clobbers it:

```cpp
std::optional<std::wstring> UpsertJsonNumberKey(std::wstring_view existing, std::wstring_view key, std::optional<double> value); // PURE RMW core (unit-tested)
std::wstring               ClaudeUserSettingsPath();             // <CLAUDE_CONFIG_DIR | ~/.claude>/settings.json
std::optional<int64_t>     GetClaudeCleanupPeriodDays();         // nullopt => key absent / file missing
bool                       SetClaudeCleanupPeriodDays(std::optional<int64_t>); // nullopt removes the key; RMW
```

- **Read-modify-write that preserves every other key** (insertion order kept) and **refuses to overwrite
  a non-empty file it can't parse** — a hand-edited `settings.json` is never destroyed.
- **Pretty-printed** (2-space) on write, so the file stays human-readable (unlike the compact `json::Dump`).
- The cog field reads via `GetClaudeCleanupPeriodDays` on open and writes via `SetClaudeCleanupPeriodDays`
  on Save: **blank ⇒ remove our key** (revert to Claude's 30-day default), a pure integer ⇒ set it, other
  text ⇒ leave the file untouched.

### Tests (`tests/m5_tests.cpp`)

`ApplyEnvDefaults` (seed-from-0, no-re-seed-when-current, no-duplicate-NAME case-insensitive,
fresh-line append) and `UpsertJsonNumberKey` (set on empty, preserve other keys + single member on
update, `nullopt` removes, refuse to clobber an unparseable / non-object file).
