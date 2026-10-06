# Harnesses — Claude Code on other models (proposal)

> **Status: PROPOSAL (2026-10-06). Not implemented.** Decisions D1–D9 (§11) are open. Read against
> HEAD `721b94d9b`. The reference is the user's `mimo` / `mimo-flash` launcher
> (`~/.claude/scripts/openrouter.ps1` + `openrouter-profiles.json`, verified 2026-10-06 on Claude Code
> 2.1.282). Companions: [`ENV_VARS.md`](./ENV_VARS.md) (env layers) · [`HOOKS.md`](./HOOKS.md) (the
> hooks file) · [`RESTART.md`](./RESTART.md) (launch options on restart, D4) ·
> [`PROFILES.md`](./PROFILES.md) (the state profile).

## 1. Goal

Today Agentmaster starts every managed session the same way: on the models and the login of the user's
own Claude setup, with at most a `--model` pick. The goal: start a session on another model type (first Xiaomi MiMo-V2.6 Pro and Flash) through a provider the user
picks (OpenRouter, or Xiaomi's own API), and keep the full Claude Code harness: hooks, skills, MCP
servers, CLAUDE.md files, and every Agentmaster feature.

| # | Requirement (the user, 2026-10-06) |
|---|---|
| Q1 | One harness per model type: a launch configuration (models, limits, request body, arguments). |
| Q2 | Both OpenRouter and Xiaomi directly. |
| Q3 | A value typed in Settings wins over Agentmaster's own env vars (Settings → Environment variables), which win over the external (Windows) environment. |
| Q4 | An empty field shows the value that applies instead, as a gray placeholder. |

## 2. The reference: what `mimo` does

Per launch, `Invoke-ClaudeOpenRouter` does five things:

1. It reads the profile (`model`, `smallModel`, `switchModel`, `extraBody`). An `OPENROUTER_*` knob
   that is set wins over the profile.
2. It fetches `https://openrouter.ai/api/v1/models/<id>/endpoints` (public, 15 s timeout) and takes the
   smallest `context_length` and `max_completion_tokens` among the providers in `provider.order`.
   Max output = min(131,072, that provider maximum, context ÷ 4); 32,000 when the fetch fails.
3. It writes one settings file per launch (`~/.claude/temp/openrouter/settings-*.json`, deleted when
   claude exits) with `model` and an `env` block. With `AGENTMASTER_PROFILE` set, it merges
   `<profile>\hooks-settings.json` into that file.
4. It sets `ANTHROPIC_AUTH_TOKEN` (the key), `ANTHROPIC_API_KEY` (empty) and `ANTHROPIC_BASE_URL` in
   the process environment, and restores them after claude exits.
5. It runs `claude.exe --settings <file> --append-system-prompt <identity note> <args>`.

The generated block (`OPENROUTER_DRY_RUN=1`, profile `mimo`, 2026-10-06):

```json
{
  "model": "xiaomi/mimo-v2.6-pro",
  "env": {
    "ANTHROPIC_BASE_URL": "https://openrouter.ai/api",
    "ANTHROPIC_API_KEY": "",
    "ANTHROPIC_MODEL": "xiaomi/mimo-v2.6-pro",
    "ANTHROPIC_DEFAULT_FABLE_MODEL": "xiaomi/mimo-v2.6-pro",
    "ANTHROPIC_DEFAULT_FABLE_MODEL_NAME": "MiMo-V2.6-Pro (OpenRouter)",
    "ANTHROPIC_DEFAULT_OPUS_MODEL": "xiaomi/mimo-v2.6-pro",
    "ANTHROPIC_DEFAULT_OPUS_MODEL_NAME": "MiMo-V2.6-Pro (OpenRouter)",
    "ANTHROPIC_DEFAULT_SONNET_MODEL": "xiaomi/mimo-v2.6-pro",
    "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME": "MiMo-V2.6-Pro (OpenRouter)",
    "ANTHROPIC_DEFAULT_HAIKU_MODEL": "xiaomi/mimo-v2.6-pro",
    "ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME": "MiMo-V2.6-Pro (OpenRouter)",
    "CLAUDE_CODE_SUBAGENT_MODEL": "xiaomi/mimo-v2.6-pro",
    "CLAUDE_CODE_MAX_OUTPUT_TOKENS": "131072",
    "CLAUDE_CODE_MAX_CONTEXT_TOKENS": "1048576",
    "ANTHROPIC_CUSTOM_MODEL_OPTION": "xiaomi/mimo-v2.6-flash",
    "ANTHROPIC_CUSTOM_MODEL_OPTION_NAME": "MiMo-V2.6-Flash (OpenRouter)",
    "ANTHROPIC_CUSTOM_MODEL_OPTION_DESCRIPTION": "xiaomi/mimo-v2.6-flash through OpenRouter, same peak settings",
    "CLAUDE_CODE_EXTRA_BODY": "{\"provider\":{\"order\":[\"gmicloud\",\"novita\",\"deepinfra\"],\"quantizations\":[\"bf16\",\"fp8\"]},\"temperature\":1.0,\"top_p\":0.95}"
  }
}
```

Claude Code caps the output of an unrecognized model at 128,000 tokens, so 131,072 sends 128,000.

### 2.1 Facts that shape the design

| # | Fact | Source | Consequence |
|---|---|---|---|
| F1 | Claude Code copies the `env` block of its settings files over its process environment at startup. `--settings` outranks the user, project and local files. | Measured 2026-10-06 (`~/.claude/CLAUDE.Bash.md`, "OpenRouter Models in Claude Code") | Harness values go into the `--settings` file. The user's settings pin `model`, `ANTHROPIC_DEFAULT_OPUS_MODEL` and `CLAUDE_CODE_MAX_OUTPUT_TOKENS=150000` (above MiMo's 131,072); a process variable cannot override them. The same limit applies to Agentmaster's env editor today ([`ENV_VARS.md`](./ENV_VARS.md) §2). |
| F2 | claude reads one `--settings` value, and Agentmaster already passes its hooks file there. | `BuildClaudeCommandline` | A harness session needs ONE file: the hooks plus the harness keys. |
| F3 | Claude Code watches its settings files. The launcher writes one file per launch for this reason. | `openrouter.ps1` | A shared file rewritten later changes running sessions. So: one file per session. |
| F4 | A non-Anthropic key in `ANTHROPIC_API_KEY` opens the modal "Detected a custom API key in your environment — Do you want to use this API key?", with "No (recommended)" selected. `ANTHROPIC_AUTH_TOKEN` (a Bearer token) runs unattended. | Strings in `claude.exe` 2.1.291; the launcher's sessions | Send every key as `ANTHROPIC_AUTH_TOKEN` and keep `ANTHROPIC_API_KEY` empty. An unattended tab cannot answer a modal (CLAUDE.md gotcha "An unanswerable startup modal"). |
| F5 | MiMo's thinking blocks come back with EMPTY signatures, and Claude Code sends them back every turn. | CLAUDE.Bash.md | Continuing a MiMo conversation on Anthropic models can fail (R1). |
| F6 | Claude Code tells an unknown model that it is "Claude Fable 5". | CLAUDE.Bash.md | Add an identity note with `--append-system-prompt` (the hidden `--append-system-prompt-file` also exists in 2.1.291). |
| F7 | An unknown model gets no ToolSearch (every tool, every turn: a ~35K-token prefix). `/cost` overstates 17–33×. The claude.ai connectors are off. | CLAUDE.Bash.md | Nothing to build. The harness tooltip says it. |
| F8 | MiMo is slow: ~50 tokens/s. One turn thought for 22 minutes before its first tool call. | CLAUDE.Bash.md | The pull reconcilers must keep such a silent turn Running (R5). |

## 3. Rejected: run `mimo.cmd` as the session command

Agentmaster could start `mimo.cmd` instead of `claude.exe`. It does not, for these reasons:

- **Two `--settings`.** Agentmaster passes its hooks file and the launcher passes its own. claude keeps
  one. Agentmaster would have to drop its hooks and depend on the `AGENTMASTER_PROFILE` merge.
- **Cost per launch.** Each resume starts pwsh and makes up to three endpoint fetches of up to 15 s.
  A window restore pays this once per tab.
- **The native-exe launch policy.** A `.cmd` needs `cmd /c`, and cmd asks "Terminate batch job (Y/N)?"
  after a Ctrl+C.
- **Reach.** The launcher lives in the user's private `~/.claude`. The release ships to other users.
- **No UI.** No settings page, no placeholders, no record of which harness a session used.

Agentmaster reuses the launcher's knowledge instead: the same presets, the same `OPENROUTER_*` knob
names, the same generated keys. The launcher keeps working in other terminals.

## 4. Concepts

- **Provider** — an Anthropic-compatible endpoint, its credentials and its request rules. Built in:
  OpenRouter and Xiaomi MiMo. User-defined: any other endpoint (GLM, Kimi, DeepSeek, a local proxy).
  "Anthropic" (the user's Claude login) is the absence of a harness: today's behavior.
- **Harness** — a named configuration that runs Claude Code on one model type through one provider:
  the model of each tier, limits, sampling, routing, identity note, extra env, extra arguments. One
  harness = one menu entry ("MiMo Pro · OpenRouter").
- **Launch target** — what one launch runs: `""` (Default), a model id (today's `--model` picks), or
  `@<harnessId>`. It is one string, parsed by one pure function (`ParseLaunchTarget`). It travels
  through the existing `modelOverride` plumbing unchanged: the menu callbacks (`hstring`),
  `_LaunchClaudeSession`, `_ForkManagedSessionById`, `_ForkSessionFromDisk`,
  `_RestartTabIntoFreshSession`, the /handover hint parser (`ParseHandoverArgsHints`) and the
  successor-model combos. No Anthropic, OpenRouter or Xiaomi model id starts with `@`.

Built-in presets (from `openrouter-profiles.json`, 2026-10-06):

| Id | Provider | Main and background model | `/model` switch | Request body |
|---|---|---|---|---|
| `mimo-pro.openrouter` | OpenRouter | `xiaomi/mimo-v2.6-pro` | `xiaomi/mimo-v2.6-flash` | `provider.order` gmicloud, novita, deepinfra; `quantizations` bf16, fp8; `temperature` 1.0; `top_p` 0.95 |
| `mimo-flash.openrouter` | OpenRouter | `xiaomi/mimo-v2.6-flash` | `xiaomi/mimo-v2.6-pro` | the same, order + io-net |
| `mimo-pro.xiaomi` | Xiaomi | `mimo-v2.6-pro` | `mimo-v2.6-flash` | `temperature` 1.0; `top_p` 0.95; no `provider` block |
| `mimo-flash.xiaomi` | Xiaomi | `mimo-v2.6-flash` | `mimo-v2.6-pro` | the same |

Word choice: "profile" already names the state profile ([`PROFILES.md`](./PROFILES.md),
`AGENTMASTER_PROFILE`) and Windows Terminal's profiles. "Harness" is the user's word for Claude Code.

## 5. Configuration and precedence

### 5.1 The tiers

Every harness field resolves through these tiers. The first tier that is set wins.

| Tier | Source | Example |
|---|---|---|
| 1 | The field's box (Settings → Harnesses) | `xiaomi/mimo-v2.6-flash` |
| 2 | Agentmaster's env: Per-directory, then Global (Settings → Sessions → Environment variables) | `OPENROUTER_SMALL_MODEL=…` |
| 3 | The Windows environment | the user variable `OPENROUTER_API_KEY` |
| 4 | Fetched | the OpenRouter endpoints data, cached in `model-limits.json` |
| 5 | Preset | the built-in harness (updated with each release) |

Rules:

- **"Set"** means not empty after trimming. The value `none` turns an optional value off (the switch
  model), because Windows cannot hold an empty variable. The launcher uses the same rule.
- **The request body merges.** Preset ⊕ `<P>_EXTRA_BODY` ⊕ the box: a deep merge, the later value wins
  per key, arrays replace (the launcher's rule). `OPENROUTER_PROVIDER` replaces only `provider.order`.
- **Tier 2 at spawn** is `ResolveSessionEnv(settings, dir)`, Global then Per-directory. So a
  per-directory `OPENROUTER_API_KEY` bills one folder's sessions to another key.
- **Tier 3** is Agentmaster's own process environment, which every managed session inherits
  ([`ENV_VARS.md`](./ENV_VARS.md) §1). It is a snapshot from app start. For the provider's own names
  only, a name that the process does not have is read from the registry (HKCU, then HKLM
  `Environment`), so a key set after the start works without a restart. When both hold a value and
  the values differ, the settings page shows a warning.
- **One resolver.** `ResolveHarnessField(spec, box, agentmasterEnv, windowsEnv, fetched, preset)`
  returns `{value, source}`. The placeholder AND the spawn call it, so they cannot disagree (the
  `PickCurrentPromptText` idiom).

### 5.2 Fields

`<P>` is the provider's env prefix: `OPENROUTER`, `MIMO`, or the prefix of a custom provider.

| Field | Env name (tiers 2–3) | Becomes |
|---|---|---|
| Base URL | `<P>_BASE_URL` | settings `env.ANTHROPIC_BASE_URL` |
| API key | `<P>_API_KEY` | process `ANTHROPIC_AUTH_TOKEN` (§6.3); settings `env.ANTHROPIC_API_KEY = ""` |
| Main model | — (tier 1 or 5) | settings `model`; `env.ANTHROPIC_MODEL`, `ANTHROPIC_DEFAULT_{FABLE,OPUS,SONNET}_MODEL`, `CLAUDE_CODE_SUBAGENT_MODEL` |
| Background model | `<P>_SMALL_MODEL` | `env.ANTHROPIC_DEFAULT_HAIKU_MODEL` |
| `/model` switch | `<P>_SWITCH_MODEL` | `env.ANTHROPIC_CUSTOM_MODEL_OPTION` (+ `_NAME`, `_DESCRIPTION`) |
| Display names | — | `env.ANTHROPIC_DEFAULT_*_MODEL_NAME` (fetched name, else the id) |
| Context window | `<P>_CONTEXT` | `env.CLAUDE_CODE_MAX_CONTEXT_TOKENS` |
| Max output | `<P>_MAX_OUTPUT` | `env.CLAUDE_CODE_MAX_OUTPUT_TOKENS` |
| Routing (OpenRouter only) | `OPENROUTER_PROVIDER` | `env.CLAUDE_CODE_EXTRA_BODY.provider` |
| Sampling | — | `env.CLAUDE_CODE_EXTRA_BODY.temperature`, `.top_p` |
| Extra body (JSON) | `<P>_EXTRA_BODY` | merged into `env.CLAUDE_CODE_EXTRA_BODY` |
| Extra env | — | the last keys of `env` (they win over the generated ones) |
| Identity note | — | `--append-system-prompt "<text>"` (a template with `{name}`, `{model}`, `{provider}`, `{switchName}`, `{switchModel}`) |
| Extra arguments | — | appended claude flags; the lexer refuses `--settings`, `--session-id`, `--resume`, `--fork-session`, `--model` |

### 5.3 Placeholders

- An empty box shows `<value>  ← <source>` as its gray `PlaceholderText`, for example
  `xiaomi/mimo-v2.6-flash  ← OPENROUTER_SMALL_MODEL (Windows)`.
- A key is masked: `sk-or-v1-…a1b2  ← OPENROUTER_API_KEY (Windows)`.
- The settings page's hover panel (the LocalTooltip surface) lists the whole chain and marks the
  winner.
- The settings page has no folder, so placeholders use the Global env. The panel says when a
  Per-directory entry changes the value for some folders.
- A filled box keeps its value. Its panel shows the value that applies when the box is cleared.
- Placeholders recompute when the Global env editor changes and at each open of the settings page
  (Windows env, registry, Credential Manager).

### 5.4 Secrets

- A typed key goes to Windows Credential Manager (a generic credential, target
  `Agentmaster/<profile-hash>/<providerId>`), never to `settings.json`. Credential Manager protects it
  with the user's Windows login (DPAPI inside), keeps every byte of it out of the profile folder, and
  lets the user see or delete it in Control Panel → Credential Manager. The launch seam reads it
  (`CredReadW`); the pure builders get the resolved value. The link change is one
  `#pragma comment(lib, "advapi32.lib")` in that TU (the `PortableInstall.h` idiom).
- A key never appears in hooks.log, `[nav]` lines, the catalog, the CLI, or the launch preview (masked).
- The env editor's lexer warns on a `*_API_KEY` line: the value is plain text in `settings.json`.
- A copied profile folder does not carry the credential. The key then resolves from the env, or the
  launch reports it missing.

## 6. What a harness launch produces

### 6.1 The settings file

`<profile>\harness-settings\<sessionId>.json` holds the hooks, the settings page's keys, and the
harness keys:

```json
{
  "hooks": { "…": "the same object as hooks-settings.json" },
  "model": "xiaomi/mimo-v2.6-pro",
  "env": { "…": "the block of §2, then the harness's extra env" }
}
```

- `BuildHooksSettingsJson` gains an optional env block, so the hooks are built in one place. An empty
  block keeps `hooks-settings.json` byte-identical.
- The keys `includeCoAuthoredBy` and `permissions.defaultMode` come from the same builder as today.
- The builder computes the path and the JSON (pure, unit-tested in `tests/`). The launch seam writes the
  file, next to `PrepareManagedClaudeWorkspace`.
- Each launch of the session rewrites the file. An edited harness therefore applies at the session's
  next start, never during a run (F3). Close deletes the file. Engine init deletes the files of
  sessions that are not live; every relaunch writes its file again.

### 6.2 The command line

- `--settings "<harness file>"` replaces the hooks file. No `--model`: the file pins every tier.
- `--append-system-prompt "<note>"`, PowerShell-quoted (`PsDoubleQuote`), before any positional
  prompt.
- Then the harness's extra arguments.

### 6.3 The environment and the token

The ConPTY env (`spec.env`, after `CCMGR_*` and the user's env) gets `AM_HARNESS=<id>` (the observer
stamp) and `AM_HARNESS_TOKEN=<key>`. The pwsh host script (`BuildPwshHostedCommandline`) moves the
token into `ANTHROPIC_AUTH_TOKEN` for the claude call only:

```powershell
$env:ANTHROPIC_AUTH_TOKEN = $env:AM_HARNESS_TOKEN; Remove-Item Env:AM_HARNESS_TOKEN
try { & "C:\…\claude.exe" --dangerously-skip-permissions --settings "…/harness-settings/<sid>.json" --session-id <sid> --append-system-prompt "Model identity: …" }
finally { Remove-Item Env:ANTHROPIC_AUTH_TOKEN -ErrorAction Ignore }
```

Why: the tab keeps a pwsh prompt after claude exits (`-NoExit`). Without this, a plain `claude` typed
at that prompt sends the OpenRouter key to Anthropic and fails with 401. The launcher restores its
variables for the same reason.

### 6.4 Refusals and logs

- No key: a launch from the UI shows a modal (the `_PromptClaudeMissing` idiom: "OpenRouter key
  missing: set it in Settings ▸ Harnesses or in OPENROUTER_API_KEY"). A restore path logs
  `[launch-blocked] harness <id>: …` and keeps the session resumable.
- Logs name the harness, never the key: `[spawn] <sid> … harness=mimo-pro.openrouter` and
  `[nav] open-new claude … model=@mimo-pro.openrouter`.

## 7. A harness is sticky per session

`SessionInfo::launchTarget` is persisted in `sessions.json`, and is omitted when empty, so old files
stay byte-identical. Only an `@harness` target persists; a plain `--model` pick stays per launch, as
today (D5).

| Seam | Today | With harnesses |
|---|---|---|
| New session (menus, launch bar) | the `--model` pick, this launch only | the pick; an `@harness` target is recorded |
| Resume, Sessions "Resume here", window restore, Deactivate wake, restore re-fork | the Settings model | the recorded target |
| Restart session (`BuildClaudeRestartSpec`) | drops `--model` ([`RESTART.md`](./RESTART.md) defect 5) | the recorded target; RESTART.md D4 reads it from the record, not from the old command line |
| Fork (tab, board, Sessions page) | "Default" = the Settings model | "Default" reads "Same as source (MiMo Pro · OpenRouter)"; another pick overrides |
| /handover successors | message hint > per-command combo > Default | message hint > per-command combo (not Default) > the origin's target > Default |
| /clear successor (same process) | a new record | copy the predecessor's target where `OnHookEvent` pairs `SessionStart(clear)` with `SessionEnd(clear)` |
| Adopt a hand-typed `mimo` | resumes on Anthropic | P5: infer the harness from the PEB `ANTHROPIC_BASE_URL` and the transcript's model; else ask |

A switch to another provider happens only when the user picks it ("Resume on ▸" or a Fork entry), with
a tooltip that names risk R1.

## 8. User interface

### 8.1 Settings → Harnesses (a new tab)

Every box below is EMPTY. The text inside the brackets is its gray placeholder (Q4): the value that
applies, then its source.

```
PROVIDERS
 OpenRouter                                                              [Test key]
   Base URL  [ https://openrouter.ai/api  ← preset                            ]
   API key   [ sk-or-v1-…a1b2  ← OPENROUTER_API_KEY (Windows)             (o) ]
   ✓ key valid (<key label>, <usage> used)
 Xiaomi MiMo                               Endpoint [Pay-as-you-go ▾]
   Base URL  [ https://api.xiaomimimo.com/anthropic  ← preset                 ]
   API key   [ none: type a key, or set MIMO_API_KEY                      (o) ]
   ⚠ no key: the Xiaomi harnesses are hidden from the launch menus

HARNESSES                                                    [+ New] [Duplicate]
 [x] MiMo Pro · OpenRouter      [ ] MiMo Pro · Xiaomi
 [x] MiMo Flash · OpenRouter    [ ] MiMo Flash · Xiaomi
 Editing: MiMo Pro · OpenRouter                                   [Fetch limits]
   Main model        [ xiaomi/mimo-v2.6-pro  ← preset                         ]
   Background model  [ xiaomi/mimo-v2.6-pro  ← preset                         ]
   /model switch     [ xiaomi/mimo-v2.6-flash  ← preset   (none = no entry)   ]
   Context window    [ 1048576  ← fetched 2026-10-06                          ]
   Max output        [ 131072  ← fetched (Claude Code sends at most 128,000)  ]
   Provider order    [ gmicloud, novita, deepinfra  ← preset                  ]
   Quantizations     [ bf16, fp8  ← preset                                    ]
   Temperature/top_p [ 1.0  ← preset ] [ 0.95  ← preset ]
   Extra body (JSON) [ merged over the fields above                           ]
   Extra env         [ NAME=VALUE, one per line (live-lexed)                  ]
   Identity note     [ Model identity: this session runs on {name} …  ← preset ]
   Extra arguments   [ e.g. --autocompact 600000                              ]
   Launch preview (key masked): the settings JSON + the command line
```

- **Test key** (OpenRouter): `GET /api/v1/key`. It uses no tokens.
- **Fetch limits** (OpenRouter): the launcher's endpoints rule, cached in `model-limits.json` (tier 4).
  Never during a launch: a launch uses the cache or the preset and refreshes in the background.
- The launch preview replaces `OPENROUTER_DRY_RUN=1`.

### 8.2 Menus and the launch bar

- `LaunchTargetEntries(settings)` = the `launchModels` entries, then the visible harnesses as
  `{name, "@id"}`. It replaces `ParseLaunchModels(_appSettings.launchModels)` in the menu builders and
  the successor-model combos (nine call sites today); the "Specify…" cards keep the plain list.
  `ParseHandoverArgsHints` gets the same list, so `/handover [mimo] …` picks the harness.
- `AgentFillModelPickItems` puts the harnesses in their own group, after a separator, with their own
  tooltip.
- A harness is visible when it is enabled AND its provider resolves a key (D6). With
  `OPENROUTER_API_KEY` set, the two OpenRouter presets appear with no setup.
- `AppSettings::defaultLaunchTarget` (D9) drives the launch bar's Launch button and every "Default"
  item of the "Open New Session Here" menus.

### 8.3 Display

- The board card, the per-tab badge and the tab tooltip show a pill `MiMo · OR` (the teal `codex` pill
  idiom).
- `ShortModelName` drops a `vendor/` prefix: `xiaomi/mimo-v2.6-pro` → `mimo-v2.6-pro`. Before the first
  turn, the observer reads `AM_HARNESS` for the model text (`ParseClaudeFacts` reads `ANTHROPIC_MODEL`
  from the process env, and a harness puts it in the settings file).
- The ⚡ cache hint uses a per-provider `cacheMinutes`; empty hides it. Anthropic's 5-minute rule does
  not describe GMICloud or Xiaomi.
- The Sessions page shows a harness chip. "Resume here" uses the recorded target; "Resume on ▸" offers
  a switch.
- The CLI's `show` prints `harness:`; `--json` carries `launchTarget`.

## 9. Provider notes

**OpenRouter**

- Base URL `https://openrouter.ai/api` (claude appends `/v1/messages`). Bearer token. Prefix
  `OPENROUTER`.
- The `provider` routing block (`order`, `quantizations`, `allow_fallbacks`) exists only here.
- Limits: `GET /api/v1/models/<id>/endpoints` (public: 200 without a key on 2026-10-06). The parser is
  pure, in `ModelCatalog.h`, tested with recorded fixtures.
- Key check: `GET /api/v1/key` (401 without a key on 2026-10-06).
- `GET /api/v1/models` (public) can feed the "Specify a model" list for an ad-hoc
  `@openrouter:<model>` target (P5, the launcher's `claude-openrouter MODEL`).
- The account's privacy settings decide which providers may serve. A blocked route comes back as an
  API error, which the scanner already shows (`recon-error` → the Error card with the HTTP status).

**Xiaomi MiMo**

- Pay-as-you-go: `https://api.xiaomimimo.com/anthropic`, keys `sk-…`. Token Plan:
  `https://token-plan-cn.xiaomimimo.com/anthropic` (or the region URL from the plan page), keys `tp-…`
  and `ttp-…`. A key works only with its own URL: the lexer warns on a mismatch.
- Prefix `MIMO`. Xiaomi's own examples use `MIMO_API_KEY`; the other `MIMO_*` names are Agentmaster's.
- Model ids have no vendor prefix: `mimo-v2.6-pro`, `mimo-v2.6-flash`.
- No `provider` block: it is an OpenRouter field, and an unknown field can cause a 400. Xiaomi forces
  temperature 1.0 and top_p 0.95 in thinking mode, so sending them is harmless.
- Auth header: community configurations use `ANTHROPIC_AUTH_TOKEN` (Bearer). Xiaomi's curl example
  sends an `api-key` header and its SDK example `x-api-key`. P0 checks Bearer. If only `x-api-key`
  works, F4 applies: pre-approve the key in `~/.claude.json` `customApiKeyResponses.approved` at the
  launch seam (the workspace-trust idiom), or use `apiKeyHelper`.
- Pro-UltraSpeed is served only by Xiaomi's endpoint.

**Custom** — a name, a base URL, an env prefix and an auth style. No routing block.

## 10. Risks and checks before code (P0)

R1–R4, R6 and R7 run without an Agentmaster build: the `claude-wrap-ConPTY` skill's driver, a
throwaway folder, a hand-made settings file, and for R6 the host script run by hand. R5 needs a dev
build (P4).

| # | Risk | Check |
|---|---|---|
| R1 | Cross-provider history: MiMo's empty thinking signatures sent to Anthropic, or Anthropic's signed (or redacted) thinking sent to MiMo. | Fork a two-turn MiMo session onto Default and send a prompt; then the reverse. Record success or the API error. The result decides whether "Resume on ▸" and a cross-provider Fork are offered, warned, or blocked. |
| R2 | A startup modal parks the tab (F4). | Launch each provider on a PTY with the generated file and env. The screen must show the input box. |
| R3 | Xiaomi's auth header. | One `-p` call per header style against the pay-as-you-go URL. |
| R4 | The identity note after `--resume` and `--fork-session`. | Ask "which model are you?" after a resume. |
| R5 | A long silent MiMo turn (F8) demoted from Running. | One long turn on a dev build: no `[recon-stop]` before the real Stop. |
| R6 | The pwsh prompt after claude exits keeps the key. | At that prompt, `$env:ANTHROPIC_AUTH_TOKEN` is empty. |
| R7 | The transcript's `message.model` for each provider. | Read one assistant line. |

## 11. Decisions

| # | Decision | Options | Proposed |
|---|---|---|---|
| D1 | Name | harness / model setup / backend | **harness** in the UI (the user's word); `ModelHarness` in code, because "harness" in this repo's code and docs already means the test harness in `tests/` |
| D2 | Integration | native (§6) / run the launcher | **native** |
| D3 | Precedence | §5.1 (box > Agentmaster env > Windows env > fetched > preset) | **§5.1** |
| D4 | Typed keys | Credential Manager / DPAPI in `settings.json` / plain text | **Credential Manager** |
| D5 | Persistence | harness targets only / every model pick / none | **harness targets only** |
| D6 | Menu visibility | enabled AND a key resolves / enabled only | **enabled AND a key** |
| D7 | Missing harness or key at relaunch | refuse and keep it resumable / fall back to Default | **refuse** (a silent fallback changes the model and the bill, and can hit R1) |
| D8 | Settings file | per session / per harness (shared and watched) / per launch | **per session** |
| D9 | Default target | `defaultLaunchTarget` / the Model box accepts `@id` | **`defaultLaunchTarget`** |

## 12. Plan

- **P0 — checks.** R1–R4, R6, R7 (§10). Builds nothing.
- **P1 — engine, pure, with unit tests.** A new header-only `AgentMaster/ModelHarness.h` (the
  `ModelCatalog.h` idiom): `ProviderSpec`, `HarnessSpec`, the presets, the field table,
  `ResolveHarnessField`, `ComposeExtraBody`, `BuildHarnessEnv`, `ParseLaunchTarget`, `MaskSecret`, the
  lexers. `ParseOpenRouterEndpoints` goes into `ModelCatalog.h`. `BuildHooksSettingsJson` takes the
  optional env block; `BuildClaudeSpawn` and `BuildClaudeRestartSpec` take a resolved harness;
  `BuildPwshHostedCommandline` scopes the token. Persistence: `SessionInfo::launchTarget`,
  `AppSettings::providers`, `harnesses` (sparse overrides of the presets, so an untouched preset follows
  each release), `defaultLaunchTarget`. Tests: the precedence matrix, empty and `none`, the body merge,
  the OpenRouter and Xiaomi env blocks (no `provider` block for Xiaomi), hooks identical to
  `hooks-settings.json`, the note's quoting before a positional prompt, the persistence round trip
  (absent field → byte-identical), the endpoints fixtures.
- **P2 — launch plumbing.** The target through the menu sites, the sticky seams (§7), the refusals,
  `AM_HARNESS`, the Credential Manager read, the settings-file lifecycle.
- **P3 — the settings tab.** Providers, the harness editor, placeholders, the chain panel, Test key,
  Fetch limits, the launch preview, the env lexer's key warning.
- **P4 — display and docs.** The pill, `ShortModelName`, the per-provider ⚡, the Sessions chip, the
  CLI field. This file becomes the reference; ENV_VARS.md and CLAUDE.md follow. Live check R5.
- **P5 — later.** Adopt a hand-typed `mimo`, the ad-hoc `@openrouter:<model>` target, an import of
  `openrouter-profiles.json`, providers for Codex.

Files: `AgentMaster/ModelHarness.h` (new), `ClaudeSpawn.{h,cpp}`, `ModelCatalog.h`, `SessionModels.h`,
`Persistence.cpp`, `SessionRegistry.cpp`, `ProcessInspect.cpp`, `AgentModelMenu.h`,
`AgentManagerContent.Settings.cpp`, `AgentManagerContent.Tree.cpp`, `TerminalPage.AgentSessions.cpp`,
`TerminalPage.AgentSessionsPage.cpp`, `TerminalPage.cpp`, `cli/agentcli.cpp`, `tests/`.
