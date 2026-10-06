# Presets — Claude Code sessions on other models and providers

> **Status: IMPLEMENTED v1 (2026-10-06), NOT BUILT.** Every `TerminalAppLib` file (89) passes a `/Zs`
> syntax check with the last build's recorded flags (`/W3 /WX` + the `/we` list), and every
> `agentmaster-cli` file (16) with its own (`/W4 /WX`), negative controls included. The engine harness
> passes `TestLaunchPresets` (248 checks) and the host-script checks in `tests_spawn_sched.cpp`. No live
> session has run on this code yet: the P0 checks (§10) and the UI checks (§12) are open. The file keeps
> its proposal-time name (`HARNESSES.md`), because code comments cite its sections. The proposal it
> replaces called the feature "harnesses"; the user renamed it **Presets** on 2026-10-06 (§11).
>
> The reference configuration is the user's OpenRouter launcher (`mimo`, `mimo-flash`, `kimi`, `qwen`,
> `glm`: `~/.claude/scripts/openrouter.ps1` + `openrouter-profiles.json`, verified 2026-10-06 on Claude
> Code 2.1.282, `mimo-flash` re-checked on 2.1.291). Companions: [`ENV_VARS.md`](./ENV_VARS.md) (the env
> chain, tier 2) · [`HOOKS.md`](./HOOKS.md) (the hooks file) · [`RESTART.md`](./RESTART.md) (relaunch
> options) · [`PROFILES.md`](./PROFILES.md) (the state profile).

## 1. Goal

Before presets, Agentmaster started every managed session on the models and the login of the user's own
Claude setup, with at most a `--model` pick. A **preset** starts a session on another model type (Xiaomi
MiMo-V2.6 Pro and Flash first, then Moonshot Kimi K3, Qwen3.8-2.4T-A95B and Z.ai GLM-5.3) through a
provider the user picks (OpenRouter, Xiaomi's own API, or any other Anthropic-compatible endpoint), and
keeps the full Claude Code harness: hooks, skills, MCP servers, CLAUDE.md files, and every Agentmaster
feature.

| # | Requirement (the user, 2026-10-06) | Where |
|---|---|---|
| Q1 | One configuration per model type: models, limits, request body, arguments. | §4, §5.2 |
| Q2 | Both OpenRouter and Xiaomi directly. | §9 |
| Q3 | A value typed in Settings wins over Agentmaster's own env vars (Settings → Environment variables), which win over the external (Windows) environment. | §5.1 |
| Q4 | An empty field shows the value that applies instead, as a gray placeholder. | §5.3 |
| Q5 | Presets are listed at the bottom of every "Fork" and "New Session Here" menu, after a separator, and can be renamed. | §8.2 |
| Q6 | The Launch button opens the same menu on hover: Default, the models, the presets. | §8.2 |
| Q7 | Add, rename, delete; tested templates shipped ahead of time; "Claude, with Codex in mind". | §4, §8.1 |
| Q8 | Templates show in the menus unless the user hides them. | §4 |
| Q9 | The session keeps the configuration it started with, FLATTENED to values (no template link). Env values are read again from the environment; values from Settings are persisted. Stored in Agentmaster's own session store, as `launchArgs`. | §7 |
| Q10 | `~/.claude/settings.json` outranking Agentmaster's env vars is the intended order. | [`ENV_VARS.md`](./ENV_VARS.md) §2 |

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

The "MiMo Pro · OpenRouter" template compiles to this block. The harness pins it key by key
(`TestLaunchPresets`); the only differences are the `*_NAME` display names (the launcher fetches
OpenRouter's names, Agentmaster derives `mimo-v2.6-pro (OpenRouter)` offline) and the number text
(`1` for `1.0`, the same JSON value). Unlike step 5, Agentmaster appends no identity note (§5.2). Claude
Code caps the output of an unrecognized model at 128,000 tokens, so 131,072 sends 128,000.

The same launcher runs `mimo-flash`, `kimi`, `qwen` and `glm`; only the profile changes. The harness
compiles every OpenRouter template and compares its `CLAUDE_CODE_EXTRA_BODY` with the profile's
`extraBody` as ordered JSON, plus its models and its limits (§4).

**Effort.** The launcher sets no effort. Every test ran with `CLAUDE_CODE_EFFORT_LEVEL=max` from the
user's own `~/.claude/settings.json`, and the evaluation lists it as a requirement for all five models
("Effort max in your settings"). The templates therefore carry `max` themselves (F9, F10), so a machine
without that variable runs the tested configuration too. Agentmaster's file adds one key the launcher's
does not: `"CLAUDE_CODE_EFFORT_LEVEL": "max"`.

### 2.1 Facts that shape the design

| # | Fact | Source | Consequence |
|---|---|---|---|
| F1 | Claude Code copies the `env` block of its settings files over its process environment at startup. `--settings` outranks the user, project and local files. | Measured 2026-10-06 (`~/.claude/CLAUDE.Bash.md`, "OpenRouter Models in Claude Code") | Preset values go into the `--settings` file. The user's settings pin `model`, `ANTHROPIC_DEFAULT_OPUS_MODEL` and `CLAUDE_CODE_MAX_OUTPUT_TOKENS=150000` (above MiMo's 131,072); a process variable cannot override them. |
| F2 | claude reads one `--settings` value, and Agentmaster already passes its hooks file there. | `BuildClaudeCommandline` | A preset session's file carries the hooks AND the preset keys. |
| F3 | Claude Code watches its settings files. The launcher writes one file per launch for this reason. | `openrouter.ps1` | A file that a running session reads is never rewritten (§6.1). |
| F4 | A non-Anthropic key in `ANTHROPIC_API_KEY` opens the modal "Detected a custom API key in your environment — Do you want to use this API key?", with "No (recommended)" selected. `ANTHROPIC_AUTH_TOKEN` (a Bearer token) runs unattended. | Strings in `claude.exe` 2.1.291; the launcher's sessions | Every key goes into `ANTHROPIC_AUTH_TOKEN`, and the file sets `ANTHROPIC_API_KEY` empty. An unattended tab cannot answer a modal (CLAUDE.md gotcha "An unanswerable startup modal"). |
| F5 | MiMo's thinking blocks come back with EMPTY signatures, and Claude Code sends them back every turn. | CLAUDE.Bash.md | Continuing a MiMo conversation on Anthropic models can fail (R1). |
| F6 | Claude Code tells an unknown model that it is "Claude Fable 5". | CLAUDE.Bash.md | Cosmetic only: no template corrects it (the user, 2026-10-06). A preset can add an identity note (`--append-system-prompt`, §5.2). |
| F7 | An unknown model gets no ToolSearch (every tool, every turn: a ~35K-token prefix). `/cost` overstates 17–33×. The claude.ai connectors are off. | CLAUDE.Bash.md | Nothing to build. |
| F8 | MiMo is slow: ~50 tokens/s. One turn thought for 22 minutes before its first tool call. | CLAUDE.Bash.md | The pull reconcilers must keep such a silent turn Running (R5). |
| F9 | `CLAUDE_CODE_EFFORT_LEVEL` overrides `--effort` and `/effort` for the whole session (`/effort` answers "CLAUDE_CODE_EFFORT_LEVEL=… overrides this session"). `unset` and `auto` turn the override off; an invalid value is ignored. | `claude.exe` 2.1.291 code (`Nz()`, the `/effort` command) | A preset's effort goes into the `--settings` file's env, which outranks the user's `settings.json` (F1), and also onto the command line as `--effort`. With `--effort` alone (v1 until 2026-10-06), the reference machine's `CLAUDE_CODE_EFFORT_LEVEL=max` silently overrode any level a preset's Effort box set. In such a session `/effort` cannot change the level. |
| F10 | OpenRouter maps Claude Code's effort `max` to each model's top reasoning level: `xhigh` for Qwen3.8. | `openrouter-profiles.json` comment; the 2026-10-06 tests | One level, `max`, serves all five templates. |

## 3. Rejected: run `mimo.cmd` as the session command

Agentmaster could start `mimo.cmd` instead of `claude.exe`. It does not, for these reasons:

- **Two `--settings`.** Agentmaster passes its hooks file and the launcher passes its own. claude keeps
  one. Agentmaster would have to drop its hooks and depend on the `AGENTMASTER_PROFILE` merge.
- **Cost per launch.** Each resume starts pwsh and makes up to three endpoint fetches of up to 15 s.
  A window restore pays this once per tab.
- **The native-exe launch policy.** A `.cmd` needs `cmd /c`, and cmd asks "Terminate batch job (Y/N)?"
  after a Ctrl+C.
- **Reach.** The launcher lives in the user's private `~/.claude`. The release ships to other users.
- **No UI.** No settings page, no placeholders, no record of which configuration a session used.

Agentmaster reuses the launcher's knowledge instead: the same profiles (as templates), the same
`OPENROUTER_*` names, the same generated keys. The launcher keeps working in other terminals.

## 4. Concepts

- **Provider** — an Anthropic-compatible endpoint and its key. Built in: **OpenRouter** (env prefix
  `OPENROUTER`) and **Xiaomi MiMo** (prefix `MIMO`). Custom: any other endpoint (GLM, Kimi, DeepSeek, a
  local proxy), with its own name, env prefix and Base URL. The **Claude login** (the user's own
  account, id `anthropic`) is the absence of a provider: a preset on it changes only the model, the
  effort, limits, env and arguments.
- **Preset** — a named configuration of one Claude session: the provider, the model of each tier,
  limits, sampling, routing, an optional identity note, extra env, extra arguments (§5.2). Its fields are
  SPARSE: only what the user typed is stored, and an empty field resolves through the tiers (§5.1).
  The user adds, renames, duplicates, hides and deletes presets.
- **Template** — a tested configuration shipped with the app (`LaunchPresetTemplates`). Each one is
  seeded ONCE into the user's preset list and stays LINKED to it: an empty field takes the template's
  CURRENT value, so a release that retunes a template reaches every preset whose field the user never
  typed. A deleted template preset is never seeded again (`seededTemplates` records it), and New ▸
  From template makes a new linked copy.
- **Launch target** — what one launch runs: `""` (Default), a model id (the old `--model` pick), or
  `@<presetId>`. It is one string, so it travels through the existing `modelOverride` plumbing
  unchanged (the menu callbacks, `_LaunchClaudeSession`, `_ForkManagedSessionById`,
  `_ForkSessionFromDisk`, `_RestartTabIntoFreshSession`, the /handover hint parser and the
  successor-model combos). No Anthropic, Bedrock, Vertex, OpenRouter or Xiaomi model id starts with `@`.
- **Launch snapshot (`launchArgs`)** — the FLATTENED values a session takes at its first launch and
  reuses on every later launch (§7). A preset edit, a preset deletion or a template update never
  changes a session that exists.

The shipped templates (from `openrouter-profiles.json`, 2026-10-06), in menu order:

| Id | Name | Main and background model | `/model` switch | Context | Request body | Seeded |
|---|---|---|---|---|---|---|
| `mimo-pro.openrouter` | MiMo Pro · OpenRouter | `xiaomi/mimo-v2.6-pro` | `xiaomi/mimo-v2.6-flash` | 1,048,576 | `provider.order` gmicloud, novita, deepinfra; `quantizations` bf16, fp8; `temperature` 1.0; `top_p` 0.95 | visible (tested 2026-10-06, Claude Code 2.1.282) |
| `mimo-flash.openrouter` | MiMo Flash · OpenRouter | `xiaomi/mimo-v2.6-flash` | `xiaomi/mimo-v2.6-pro` | 1,048,320 | the same, order + io-net | visible (tested; re-checked on 2.1.291) |
| `kimi.openrouter` | Kimi K3 · OpenRouter | `moonshotai/kimi-k3` | — | 1,048,576 | `provider.order` decart, modal, deepinfra; `quantizations` mxfp4; `temperature` 1.0; `top_p` 1.0 | visible (tested) |
| `qwen.openrouter` | Qwen3.8 2.4T · OpenRouter | `qwen/qwen3.8-2.4t-a95b` | — | 1,000,000 | `provider.order` siliconflow, novita; `require_parameters` true; `allow_fallbacks` false; `temperature` 1.0; `top_p` 0.95; `top_k` 20 | visible (tested) |
| `glm.openrouter` | GLM-5.3 · OpenRouter | `z-ai/glm-5.3` | — | 1,048,576 | `provider.order` z-ai, sail-research, siliconflow, akashml; `quantizations` fp8, bf16; `temperature` 1.0; `top_p` 1.0 | visible (tested) |
| `mimo-pro.xiaomi` | MiMo Pro · Xiaomi | `mimo-v2.6-pro` | `mimo-v2.6-flash` | 1,048,576 | `temperature` 1.0; `top_p` 0.95; no `provider` block | **hidden** (not tested: R3) |
| `mimo-flash.xiaomi` | MiMo Flash · Xiaomi | `mimo-v2.6-flash` | `mimo-v2.6-pro` | 1,048,576 | the same | **hidden** (not tested) |

All seven set Max output 131,072. The five OpenRouter templates set Effort `max`, the level their tests
ran with (§2, F9, F10); the untested Xiaomi pair sets none. None sets an identity note (§5.2). An untested
template is seeded hidden, so it never appears in a menu by surprise; the user turns on "Show in the
launch menus" to offer it. A release that adds a template seeds only that one: an existing preset list
gains it at the end, and a template the user deleted stays deleted.

### 4.1 Why these values (the evaluation of 2026-10-06)

The values come from the sessions that built the launcher profiles: each vendor's model card, OpenRouter's
endpoint list, and measured runs through Claude Code. Speed = time to the first token, then output tokens
per second, 3 samples per endpoint. Prices per million tokens on the first endpoint, as of 2026-10-06.

| Template | Precision served | First endpoint | Input / cache read / output | Balance per request |
|---|---|---|---|---|
| MiMo Pro | GMICloud bf16 (an expanded copy of Xiaomi's FP8 release); fp8 fallbacks | GMICloud: 13.0 s, then 17 tok/s | $0.435 / $0.004 / $0.87 | $0.11 |
| MiMo Flash | the same | GMICloud: 14.8 s, then 43 tok/s | $0.14 / $0.003 / $0.28 | $0.04 |
| Kimi K3 | MXFP4, the precision Moonshot trains Kimi K3 in | Decart: 1.0 s, then 201 tok/s | $2.01 / $0.201 / $10.05 | $1.29 ($1.92 on Modal) |
| Qwen3.8 2.4T | SiliconFlow fp8 (Qwen's own FP8 release); Novita states none | SiliconFlow: 2.8 s, then 43 tok/s | $2.00 / $0.25 / $6.00 | $0.77 |
| GLM-5.3 | fp8 (Z.ai's release) or bf16; Z.ai's own endpoint first | Z.AI: 2.7 s, then 83 tok/s | $1.40 / $0.26 / $4.40 | $0.56 |

- **Balance per request**: OpenRouter refuses a request when the balance cannot pay for all 128,000
  output tokens that Claude Code asks for ("This request requires more credits, or fewer max_tokens").
- **Sampling**: each vendor's benchmark settings. Kimi K3: Moonshot's (temperature 1.0, top_p 1.0).
  Qwen3.8: Qwen's recommendation (temperature 1.0, top_p 0.95, top_k 20). GLM-5.3: Z.ai's own Claude Code
  benchmark settings (temperature 1.0, top_p 1.0). MiMo: Xiaomi's (temperature 1.0, top_p 0.95).
- **Qwen's routing**: `require_parameters` skips a host that ignores `top_k` (Modal). `allow_fallbacks:
  false` keeps the request on SiliconFlow and Novita: without it, a forced failure went to Venice, which
  does not state its precision. No `quantizations` filter, because Novita states none and a filter would
  drop it. Left out: Together (it rejects 128,000 output tokens) and DeepInfra (fp4). No endpoint serves
  the BF16 original.
- **Effort**: MiMo ignores the level (200 requests changed nothing). For Kimi and GLM, 3 samples per level
  showed no clear difference between low and max, and for Qwen low used as much thinking as max. `max`
  stays because Moonshot and Z.ai benchmark at max and xhigh is Qwen's top level (F10).
- **Not applied**, as in the launcher: DeepInfra first for MiMo Pro would be about twice as fast at the
  same price, and SiliconFlow first for GLM-5.3 would halve the cost and give faster output. A preset's
  Provider order box takes either order.
- **How much the tests prove**: prices, precision and the delivery of earlier reasoning are strong; speed
  and cache are moderate (3 samples per endpoint); quality comparisons rest on the vendors' own scores and
  one math problem. Only MiMo Pro ran the graded Claude Code tasks. Not tested: long sessions near
  auto-compact, the `/model` switch, and whether Kimi, Qwen and GLM use the earlier reasoning.

"Codex in mind": a snapshot carries `agent: "claude"`, a provider entry stores an `openaiBaseUrl` beside
its Anthropic one, and a Codex snapshot is refused by this build instead of misread. Codex presets
themselves are a follow-up (§12).

## 5. Configuration and precedence

### 5.1 The tiers

Every preset field resolves through these tiers. The first tier that is set wins.

| Tier | Source | Example | In the snapshot (§7) |
|---|---|---|---|
| 1 | The field's box in the preset (Settings → Presets) | `xiaomi/mimo-v2.6-flash` | the VALUE, frozen (`set`) |
| 2 | Agentmaster's env: Global, then the launch folder's Per-directory entries (Settings → Sessions → Environment variables) | `OPENROUTER_SMALL_MODEL=…` | the VALUE, frozen (`set`) |
| 3 | The Windows environment: Agentmaster's process env, then the registry (HKCU, then HKLM `Environment`) for a name set after Agentmaster started | the user variable `OPENROUTER_SMALL_MODEL` | the variable NAME (`env`), read again at every launch |
| 4 | The linked template, or the provider's default (the Base URL) | the template's `xiaomi/mimo-v2.6-pro` | the VALUE (`base`), used when the tier-3 variable is absent at that launch |

Rules:

- **"Set"** means not empty after trimming. The value `none` turns an optional value off (the `/model`
  switch entry, the effort, the identity note), because Windows cannot hold an empty variable and an
  empty box inherits the template's value. The launcher uses the same rule.
- **One resolver.** `ResolvePresetField` (and `ResolveProviderBaseUrl` for the provider's URL) returns
  `{value, source, envName}`. The gray placeholder, the snapshot and the launch preview call it, so
  they cannot disagree (the `PickCurrentPromptText` idiom).
- **The env name** of a field is `<PREFIX>_<SUFFIX>`, where `<PREFIX>` is the provider's prefix (§5.2).
  A field with no suffix (the main model, the sampling values, the extra boxes) has no tiers 2–3.
- **The request body merges.** The routing block and the sampling fields, then the Extra request body
  JSON deep-merged over them: objects merge member by member, any other value (arrays too) replaces
  (the launcher's jq `*` rule, `PresetJsonDeepMerge`).
- **The API key** is not a field and is never stored in a snapshot. It resolves at EVERY launch:
  Windows Credential Manager (typed in Settings), then `<PREFIX>_API_KEY` in Agentmaster's env, then in
  the Windows environment (`ResolveLaunchKey`).
- **Claude Code's own settings files** (`~/.claude/settings.json` and the project files) rank above
  tiers 2–3 for every session (the intended order, [`ENV_VARS.md`](./ENV_VARS.md) §2). A preset's
  values reach claude through the `--settings` file, which ranks above them (F1).

### 5.2 Fields

The field table (`kPresetFieldSpecs`) drives the editor, the resolver, the snapshot and the compile
step, in this order. Scope: **all** = every preset; **provider** = only a preset with a provider (the
Claude login has its own models and limits); **routing** = only OpenRouter.

| Key | Label | Env name (tiers 2–3) | Scope | Becomes |
|---|---|---|---|---|
| `name` | Name | — | all | the menu label (not a launch value) |
| `provider` | Provider | — | all | a provider id; `anthropic` = the Claude login; empty = the template's |
| `model` | Main model | — | all | `--model`; the settings file's `model`; with a provider also `env.ANTHROPIC_MODEL`, `ANTHROPIC_DEFAULT_{FABLE,OPUS,SONNET}_MODEL` (+ `_NAME`), `CLAUDE_CODE_SUBAGENT_MODEL` |
| `smallModel` | Background model | `<P>_SMALL_MODEL` | provider | `env.ANTHROPIC_DEFAULT_HAIKU_MODEL` (+ `_NAME`); empty = the main model |
| `switchModel` | /model switch entry | `<P>_SWITCH_MODEL` | provider | `env.ANTHROPIC_CUSTOM_MODEL_OPTION` (+ `_NAME`, `_DESCRIPTION`); `none` = no entry |
| `effort` | Effort | — | all | `env.CLAUDE_CODE_EFFORT_LEVEL` AND `--effort`, `low|medium|high|xhigh|max`; `none` = neither |
| `context` | Context window (tokens) | `<P>_CONTEXT` | provider | `env.CLAUDE_CODE_MAX_CONTEXT_TOKENS` (digits only) |
| `maxOutput` | Max output tokens | `<P>_MAX_OUTPUT` | all | `env.CLAUDE_CODE_MAX_OUTPUT_TOKENS` (digits only) |
| `providerOrder` | Provider order | `OPENROUTER_PROVIDER` | routing | `CLAUDE_CODE_EXTRA_BODY.provider.order` |
| `quantizations` | Quantizations | — | routing | `….provider.quantizations` |
| `requireParameters` | Require parameter support | — | routing | `….provider.require_parameters` (`true` / `false`) |
| `allowFallbacks` | Allow fallbacks | — | routing | `….provider.allow_fallbacks` (`true` / `false`) |
| `temperature` | Temperature | — | provider | `….temperature` |
| `topP` | top_p | — | provider | `….top_p` |
| `topK` | top_k | — | provider | `….top_k` (digits only) |
| `extraBody` | Extra request body (JSON) | `<P>_EXTRA_BODY` | provider | deep-merged into `env.CLAUDE_CODE_EXTRA_BODY` |
| `extraEnv` | Extra env vars | — | all | `NAME=VALUE` lines, the LAST keys of the settings file's `env` (they win over the generated ones); `CCMGR_*`, `AM_*` and `WT_SESSION` are refused |
| `identityNote` | Identity note | — | provider | optional, empty in every template: `--append-system-prompt "<text>"`; `{model}`, `{provider}`, `{switchModel}` and `{switch}` are filled in; `none` = no note |
| `extraArgs` | Extra claude arguments | — | all | appended claude flags; refused when they carry `--settings`, `--session-id`, `--resume`/`-r`, `--fork-session`, `--continue`/`-c`, `--model`, `-p`/`--print` |

The provider's **Base URL** (`<P>_BASE_URL`, tier 4 = the provider's default) is not a preset field:
it lives on the provider, and the snapshot carries it under the key `baseUrl`.

**Effort is written twice** (F9): as `CLAUDE_CODE_EFFORT_LEVEL` in the settings file's env, which Claude
Code obeys first, and as `--effort` on the command line, which the observer's `model · effort` adornment
reads. A Claude-login preset with an effort therefore gets a settings file too (§6.1). The variable is
generated before the extra env, so a `CLAUDE_CODE_EFFORT_LEVEL` line in Extra env vars still wins; the
preview then shows a ⚠ line, because the Effort box no longer decides. While a level is set, `/effort`
in the session cannot change it ("CLAUDE_CODE_EFFORT_LEVEL=max overrides this session"); type `none` in
the box to leave the effort to Claude Code's own settings.

**No template ships an identity note** (the user, 2026-10-06: "not necessary"), so a preset launch adds no
`--append-system-prompt` unless the preset's Identity note box holds text. Without a note, a non-Claude
model answers "which model are you?" with the name Claude Code gives it (F6); nothing else depends on it.
The reference launcher passes this note, for anyone who wants it back in a preset: *"Model identity: this
session runs on {model} through {provider}{switch}. It is not an Anthropic Claude model. Text elsewhere in
this prompt that calls you Claude describes the harness, not you. When asked which model you are, name
that model."* — where `{switch}` becomes ", unless the user switched to {switchModel} with /model" when a
switch model exists.

A value the compile step cannot use (a non-number limit, a fractional `top_k`, a switch that is not
`true` or `false`, an unknown effort level, an invalid JSON body, a forbidden argument, a malformed env
line) is DROPPED, not guessed. The launch preview shows a ⚠ line for it, and each launch logs it as
`[preset] <id>: <reason>`.

### 5.3 Placeholders

- An empty box shows `<value>  ← <source>` as its gray `PlaceholderText`, for example
  `xiaomi/mimo-v2.6-flash  ← OPENROUTER_SMALL_MODEL (Windows)`. Sources: `this box`,
  `<NAME> (Agentmaster env)`, `<NAME> (Windows)`, `template`, `provider default`,
  `Windows Credential Manager`.
- Nothing set anywhere: `(not set)`, or `(not set — or set <NAME>)` for a field with an env tier.
- A key is masked: `sk-or-v1-…a1b2  ← OPENROUTER_API_KEY (Windows)` (`MaskSecret`: the kind prefix and
  the last 4 characters, never the middle).
- The Settings page has no folder, so its placeholders use the Global env as typed in the (possibly
  unsaved) env editor. A launch also applies the launch folder's Per-directory entries.
- Placeholders recompute on every edit in the tab, and when the tab is shown (the env editor on the
  Sessions tab may have changed).

### 5.4 Secrets

- A typed key goes to Windows Credential Manager (a generic credential, target
  `Agentmaster:provider:<providerId>`, user name `Agentmaster`, `CRED_PERSIST_LOCAL_MACHINE`). It is
  never written to a profile file. The target is not per profile: the dev and release installs share
  one typed key, the way they share the user's env vars. The user can see or delete it in Control
  Panel → Credential Manager, or with **Remove stored key** in the tab.
- The typed key lives only in the form's draft until Save; the draft is zeroed after Save.
- A key never appears in hooks.log, `[nav]` lines, the catalog, a snapshot, a launch settings file, or
  the launch preview (masked).
- A `<PREFIX>_API_KEY` in Agentmaster's Global env is plain text in `settings.json` (the env editor's
  normal behavior); the Credential Manager box is the protected place.
- A copied profile folder does not carry the credential. The key then resolves from the env, or the
  launch is refused (§6.4).

## 6. What a preset launch produces

`CompileClaudeLaunch` turns a snapshot plus this launch's tier-3 values and key into a
`CompiledClaudeLaunch`; the spawn builders apply it (`ApplyCompiledLaunch` in `ClaudeSpawn.cpp`, used by
`BuildClaudeSpawn` AND `BuildClaudeRestartSpec`, so a restart applies a preset exactly like the first
launch did).

### 6.1 The settings file

When a launch carries env values (every provider preset, and a Claude-login preset with an Effort, Max
output or Extra env), it gets its own `--settings` file instead of the shared `hooks-settings.json`:

```
<profile>\launch-settings\<presetId>-<16 hex>.json
```

```json
{
  "hooks": { "…": "the same object as hooks-settings.json" },
  "model": "xiaomi/mimo-v2.6-pro",
  "includeCoAuthoredBy": "…", "permissions": "…  (the cog's keys, exactly as in hooks-settings.json)",
  "env": { "…": "the block of §2, then the preset's extra env" }
}
```

- The hooks part is `BuildHooksSettingsJson` (the one builder), so the hooks can never differ from
  the shared file's. `BuildLaunchSettingsJson` splices the `env` block in before the final brace.
- The file's `model` is the preset's main model, else the cog's model.
- **Content-addressed and immutable**: the name's hash (FNV-1a) covers the whole content, and an
  intact file is never rewritten. A running session therefore never sees its settings file change
  (F3), and identical launches share one file. The `<presetId>` part (`model` for a plain model pick)
  only makes the folder readable.
- **Verified and atomic**: an existing file is reused only when its bytes equal the content; a file
  that does not match its name is broken and is rewritten (`[preset] the launch settings file … did not
  match its content hash; rewriting it`). Every write is temp + rename (`WriteFileUtf8Atomic`), because
  claude ignores an invalid settings file: a partial file would start the session with no hooks and no
  provider env while the provider key still rides `ANTHROPIC_AUTH_TOKEN`.
- A file that cannot be written does not stop the launch: the session starts on the shared hooks file
  WITHOUT the preset's env, and hooks.log says so (`[preset] <sid8> launch settings file missing; …`).
- Nothing deletes old files yet (§12). Each one is a few kilobytes, one per distinct configuration.

A plain model pick ("Open New Session Here ▸ Fable") compiles to `--model <id>` only: no file, exactly
the pre-preset launch.

### 6.2 The command line

`BuildClaudeCommandline` gains, after every built-in flag and BEFORE a positional prompt (claude reads
the positional only after the flags):

- `--model <main model>` (also for a provider preset: the file's `model` and the flag agree);
- `--effort <level>` (the settings file carries the same level as `CLAUDE_CODE_EFFORT_LEVEL`, which
  Claude Code obeys first — F9, §5.2);
- `--append-system-prompt "<note>"`, only when the preset's Identity note box holds text (no template sets
  one), PowerShell-quoted (`PsDoubleQuote`: the command runs under the pwsh host's `&`);
- the preset's extra arguments, verbatim (`RenderLaunchExtraArgs`).

### 6.3 The environment and the token

The ConPTY env (`spec.env`, after `CCMGR_*` and the user's env) gets `AM_PRESET=<presetId>` (an
ownership stamp, never secret) and `AM_PRESET_TOKEN=<key>`. The pwsh host script
(`BuildPwshHostedCommandline`, which every managed Claude launch uses) moves the token into
`ANTHROPIC_AUTH_TOKEN` for the claude call only:

```powershell
$env:ANTHROPIC_AUTH_TOKEN = $env:AM_PRESET_TOKEN; Remove-Item Env:AM_PRESET_TOKEN -ErrorAction Ignore
try { & "C:\…\claude.exe" --dangerously-skip-permissions --settings "…/launch-settings/mimo-pro.openrouter-<hash>.json" --session-id <sid> --model xiaomi/mimo-v2.6-pro }
finally { Remove-Item Env:ANTHROPIC_AUTH_TOKEN -ErrorAction Ignore }
```

Why: the tab keeps a pwsh prompt after claude exits (`-NoExit`). Without this, a plain `claude` typed
at that prompt would send the OpenRouter key to Anthropic. The variable name spliced into the script is
always Agentmaster's own constant, and anything but `[A-Za-z0-9_]` is refused (the script stays
injection-free). Claude's children (Bash tool, MCP servers) inherit `ANTHROPIC_AUTH_TOKEN`, as with the
launcher.

### 6.4 Refusals and logs

A preset launch is REFUSED — nothing starts — when:

- the picked preset no longer exists;
- its provider no longer exists (a custom provider deleted in Settings; `PresetLaunchRefusal`);
- no key resolves for its provider (§5.1);
- no Base URL resolves (a custom provider with an empty URL box and no `<PREFIX>_BASE_URL`).

A silent fallback to the user's Claude login would run another model on another account (D7). Per seam:

| Seam | On refusal |
|---|---|
| A launch from a menu, the Launch button, a resume, a window restore, a fork, a /handover successor | `[launch-blocked] target <@id or model>: <reason>` (or `inherited from <sid8>`), and the notice "This preset can't start" (one dialog per burst: a window restore of N keyless sessions shows one). The session record stays resumable from the Sessions page. |
| /handover-here (in place) | `[handover-here] <sid8> launch refused: …`, then the new-tab path, which refuses the same way and shows the notice. |
| Restart session | `[restart-blocked] <sid>: …` and the notice; the old connection stays. |
| Deactivate | `[deactivate] <sid8> blocked - its preset can't launch: …`; the session keeps running. |

Logs name the preset, never the key: `[spawn] <sid> "<title>" cwd=<dir> preset=mimo-pro.openrouter
model=xiaomi/mimo-v2.6-pro` (a plain pick: ` model=<id>`), `[preset] <id>: <dropped value>`, and the
Settings Save's `[nav] presets-save presets=N keys-stored=N keys-removed=N` (counts only).

## 7. A launch is sticky per session

Every pick — a preset OR a plain model — becomes the session's snapshot, `launchArgs`, a JSON string in
Agentmaster's per-session store (`<profile>\session-store\<sid>.json`, `SessionStore.h`). It holds
values, not a reference to the preset:

```json
{"v":1,"agent":"claude","preset":"mimo-pro.openrouter","presetName":"MiMo Pro · OpenRouter",
 "provider":"openrouter","providerName":"OpenRouter","routing":true,
 "set":{"topP":"0.9"},
 "env":{"baseUrl":"OPENROUTER_BASE_URL","smallModel":"OPENROUTER_SMALL_MODEL","context":"OPENROUTER_CONTEXT", "…":"…"},
 "base":{"baseUrl":"https://openrouter.ai/api","model":"xiaomi/mimo-v2.6-pro","smallModel":"xiaomi/mimo-v2.6-pro", "…":"…"},
 "key":{"credential":"openrouter","env":"OPENROUTER_API_KEY"}}
```

- `set` = tiers 1–2 (the user's values, persisted, Q9), `env` = tier-3 NAMES (read again at each
  launch, Q9), `base` = tier 4. `key` = where to look, never the key.
- `preset` / `presetName` are provenance only: the preset is never looked up again.
- A plain model pick stores `{"v":1,"agent":"claude","set":{"model":"fable"}}`.
- A Default launch stores nothing: the session follows the cog's model, as before.
- A newer schema version, or a non-Claude agent, is refused by this build (the session then launches
  on Default rather than on a misread configuration).

| Seam | Launch target |
|---|---|
| New session (menus, the Launch button's hover menu) | the pick; a new snapshot under the new id |
| Resume (Sessions "Resume here"), window restore, restore re-fork, Deactivate wake | the session's own snapshot |
| Restart session (`BuildClaudeRestartSpec`) | the session's own snapshot (the old `--model` loss, [`RESTART.md`](./RESTART.md) defect 5, is closed for snapshotted sessions) |
| Fork (tab, board, tree, Sessions page) | a pick: the pick; **Default: the SOURCE's snapshot** (a MiMo session forks onto MiMo) |
| /handover, /handover-standby successors | message hint > per-command successor combo > **the origin's snapshot** > Default |
| /handover-here | the same order; the in-place successor continues the origin's work |
| /clear successor (same claude process) | the predecessor's snapshot, found through `continuedFromId` when `continuedVia == "clear"` (up to 8 hops; a /handover link is NOT followed: a successor launched on Default resumes on Default) |

Each new id stores its own copy at launch (a deduplicated write: an identical resume writes nothing).
Switching a session to another provider happens only through an explicit pick (a Fork entry), which
risk R1 concerns.

## 8. User interface

### 8.1 Settings → Presets (a tab after Sessions)

The cog's card grew to 800 DIP for the ninth tab. Edits go into a DRAFT, written on Save and dropped on
Cancel (the Per-directory env area's form semantics).

```
PROVIDERS
 Where a preset's requests go. Leave a box empty to use the value shown in gray, …
 OpenRouter   ·   OPENROUTER_*
   Base URL (Anthropic-compatible)  [ https://openrouter.ai/api  ← provider default        ]
   API key                          [ sk-or-v1-…a1b2  ← OPENROUTER_API_KEY (Windows)        ]
   No key is stored.
 Xiaomi MiMo   ·   MIMO_*
   Base URL (Anthropic-compatible)  [ https://api.xiaomimimo.com/anthropic  ← provider default ]
   API key                          [ none: type a key, or set MIMO_API_KEY (sk-… or tp-…)  ]
   A key is stored in Windows Credential Manager.  [Remove stored key]
 GLM   ·   GLM_*                                     (a custom provider: + Name, Env variable prefix)
   …                                                 [Delete provider]
 [Add a custom provider]

PRESETS
 A preset is a named way to start a Claude session: …
 [ MiMo Pro · OpenRouter            ▾]  [New ▾] [Duplicate] [Delete]
   From the template "MiMo Pro · OpenRouter" — tested 2026-10-06 · Claude Code 2.1.282. Empty boxes follow the template.
   Show in the launch menus  [on]
   Provider [ As the template (OpenRouter) ▾ ]   (Claude login (your own account) · OpenRouter · Xiaomi MiMo · custom …)
   Name                       [ MiMo Pro · OpenRouter  ← template                      ]
   Main model                 [ xiaomi/mimo-v2.6-pro  ← template                       ]
   Background model           [ xiaomi/mimo-v2.6-pro  ← template                       ]
   …one box per field of §5.2 in scope for the provider…
 LAUNCH PREVIEW
   Settings file env:
     ANTHROPIC_BASE_URL=https://openrouter.ai/api
     …
   Command line adds: --model xiaomi/mimo-v2.6-pro
   ANTHROPIC_AUTH_TOKEN=sk-or-v1-…a1b2 (only for the claude call)
```

- **New ▾**: "Blank preset" (named "Preset N", the first free number; on the Claude login until a
  provider is picked) or "From template: <name>" (a linked copy).
- **Duplicate** copies the template link and every typed value, named "<name> (copy)".
- **Delete** removes the preset on Save. A deleted template preset stays deleted; sessions that use it
  keep their snapshot.
- **Provider**: "As the template (…)" (a linked preset only) · "Claude login (your own account)" · the
  built-in providers · the custom providers. A provider that no longer exists is listed as
  "<id> (missing)", the tested line warns, and the preview reads "Cannot launch".
- Fields outside the provider's scope are hidden (§5.2).
- **Launch preview** = what a session started NOW would get (the reference launcher's
  `OPENROUTER_DRY_RUN=1`), with the key masked and a ⚠ line per dropped value.
- **Delete provider** also removes its stored key (on Save).

### 8.2 Menus and the Launch button

Every "Open New Session Here" and "Fork session" model submenu — the board / tree session menu, the
EXTERNAL menu, the Sessions page's row menu and its detail-pane split buttons, the WT tab menu — and the
Commands tab's successor-model combos list the presets (`LaunchMenuEntries`):

```
Default
Specify…
─────────────
Fable / Opus / Sonnet          (the launch models, Settings → Sessions)
─────────────
MiMo Pro · OpenRouter          (the VISIBLE presets, in list order)
MiMo Flash · OpenRouter
Kimi K3 · OpenRouter
Qwen3.8 2.4T · OpenRouter
GLM-5.3 · OpenRouter
Preset 1
```

- A preset is visible unless its "Show in the launch menus" switch is off. A visible preset whose key
  is missing is still listed; picking it shows the refusal notice (§6.4).
- Default: a new session uses the cog's model; a fork keeps its SOURCE's model or preset (§7).
- The /handover per-message hint matches presets too, more strictly than the launch models:
  `LaunchMenuSpec` appends the visible presets to the launch-models spec as `Name | @id` lines, and a
  preset is picked only by the BRACKETED form with a PREFIX of its name (`ParseHandoverArgsHints`).
  `/handover [mimo] …` picks "MiMo Pro · OpenRouter" (the first visible match), `[mimo flash]` the
  Flash one, `[kimi]`, `[qwen]` and `[glm 5.3]` the others. A bare word never picks a preset, and a
  bracket that only occurs inside a name (`[router]`, `[flash]`) is a successor title. Why: a preset
  moves the successor to another provider and bill, and under the models' substring rule (before
  2026-10-06) `/handover open the PR`, `out of memory` or `pro tips` matched every "… · OpenRouter" name.
- **The Launch button's hover menu**: hovering "Launch Claude" for 450 ms opens the same menu under the
  button (`_ShowLaunchMenu`, `TransientWithDismissOnPointerMoveAway`: it takes no focus and closes when
  the pointer leaves). A pick launches a NEW session in the typed folder with that target; a plain
  click is Default, as before. The menu is not offered in Codex mode, while the box holds a session id
  (that is "Resume session", which keeps the session's own snapshot), or while the button is disabled.

### 8.3 Display (not built)

The board card / badge pill, `ShortModelName` without a `vendor/` prefix, a per-provider ⚡ cache rule,
a Sessions-page chip and the CLI's `preset:` line are follow-ups (§12). The observer already shows the
`--model` value of a preset session (the provider's model id) in `model · effort`.

## 9. Provider notes

**OpenRouter**

- Base URL `https://openrouter.ai/api` (claude appends `/v1/messages`). Bearer token. Prefix
  `OPENROUTER`. Key format `sk-or-v1-…`.
- The `provider` routing block (`order`, `quantizations`, `allow_fallbacks`) exists only here.
- Limits: `GET /api/v1/models/<id>/endpoints` (public: 200 without a key on 2026-10-06). Not fetched
  yet: the templates carry the launcher's values (§12).
- Key check: `GET /api/v1/key` (401 without a key on 2026-10-06). Not built yet.
- The account's privacy settings decide which providers may serve. A blocked route comes back as an
  API error, which the scanner already shows (`recon-error` → the Error card with the HTTP status).

**Xiaomi MiMo**

- Pay-as-you-go: `https://api.xiaomimimo.com/anthropic` (the built-in default), keys `sk-…`. Token
  Plan: `https://token-plan-cn.xiaomimimo.com/anthropic` (or the region URL from the plan page), keys
  `tp-…` and `ttp-…` — type that URL into the provider's Base URL box. A key works only with its own URL.
- Prefix `MIMO`. Xiaomi's own examples use `MIMO_API_KEY`; the other `MIMO_*` names are Agentmaster's.
- Model ids have no vendor prefix: `mimo-v2.6-pro`, `mimo-v2.6-flash`.
- No `provider` block: it is an OpenRouter field, and an unknown field can cause a 400. Xiaomi forces
  temperature 1.0 and top_p 0.95 in thinking mode, so sending them is harmless.
- Auth header: community configurations use `ANTHROPIC_AUTH_TOKEN` (Bearer). Xiaomi's curl example
  sends an `api-key` header and its SDK example `x-api-key`. **Unverified (R3)** — the reason the two
  Xiaomi templates are seeded hidden. If only `x-api-key` works, F4 applies: pre-approve the key in
  `~/.claude.json` `customApiKeyResponses.approved` at the launch seam (the workspace-trust idiom), or
  use `apiKeyHelper`.

**Custom** — a name, an env prefix (sanitized to `[A-Z0-9_]`, no leading digit), a Base URL and a key.
No routing block. Id `c-<8 hex>`.

## 10. Risks and checks (P0)

R1–R4, R6 and R7 run without an Agentmaster build: the `claude-wrap-ConPTY` skill's driver, a throwaway
folder, a hand-made settings file, and for R6 the host script run by hand. R5 needs a dev build. **None
has been run against this implementation yet.**

| # | Risk | Check |
|---|---|---|
| R1 | Cross-provider history: MiMo's empty thinking signatures sent to Anthropic, or Anthropic's signed (or redacted) thinking sent to MiMo. Kimi K3, Qwen3.8 and GLM-5.3 are untested in either direction, and so is a fork between two of the five models. | Fork a two-turn MiMo session onto Default and send a prompt; then the reverse; then once per other model. Record success or the API error. The result decides whether a cross-provider Fork stays offered, gets a warning, or is blocked. |
| R2 | A startup modal parks the tab (F4). | Launch each provider on a PTY with the generated file and env. The screen must show the input box. |
| R3 | Xiaomi's auth header. | One `-p` call per header style against the pay-as-you-go URL. A pass un-hides the Xiaomi templates (a template edit + its `tested` line). |
| R4 | A typed identity note after `--resume` and `--fork-session` (no template sets one). | Ask "which model are you?" after a resume. |
| R5 | A long silent MiMo turn (F8) demoted from Running. | One long turn on a dev build: no `[recon-stop]` before the real Stop. |
| R6 | The pwsh prompt after claude exits keeps the key. | At that prompt, `$env:ANTHROPIC_AUTH_TOKEN` and `$env:AM_PRESET_TOKEN` are empty. |
| R7 | The transcript's `message.model` for each provider. | Read one assistant line. |

## 11. Decisions (as taken)

| # | Decision | Taken |
|---|---|---|
| D1 | Name | **Presets** in the UI and the code (`LaunchPreset`, `LaunchPresets.h`); "harness" stays the name of the test harness in `tests/` |
| D2 | Integration | **native** (§6), not the launcher (§3) |
| D3 | Precedence | box > Agentmaster env > Windows env > template / provider default (§5.1); no "fetched" tier yet |
| D4 | Typed keys | **Windows Credential Manager**, one target per provider, shared by the installs |
| D5 | Persistence | **every pick** (preset AND plain model) as a flattened `launchArgs` snapshot in the session store — not a `SessionInfo` field, so `sessions.json` is unchanged |
| D6 | Menu visibility | the preset's own switch (untested templates start hidden); a missing key refuses at launch with a notice instead of hiding the preset |
| D7 | Missing preset, provider, key or URL at a launch | **refuse** and keep the session resumable — never fall back to the Claude login |
| D8 | Settings file | **content-addressed and immutable**, shared by identical launches (the proposal's per-session file would have been rewritten under a running session on the next relaunch) |
| D9 | Default target | not built: a plain Launch click and every "Default" item keep the cog's model |
| D10 | Templates | seeded ONCE, LINKED (empty fields follow the template), a deletion sticks |
| D11 | Template set (2026-10-06) | the launcher's five profiles as visible templates — MiMo Pro, MiMo Flash, Kimi K3, Qwen3.8 2.4T, GLM-5.3, all through OpenRouter — with their values unchanged (the evaluation's faster or cheaper orders not applied, §4.1) and Effort `max` added, the level every test ran with |
| D12 | Effort | written as `CLAUDE_CODE_EFFORT_LEVEL` in the settings file AND as `--effort` (F9); `none` = neither |
| D13 | /handover hint for presets | the bracketed form with a prefix of the preset's name only; a bare word never picks a preset (§8.2) |

## 12. Follow-ups and known gaps

- **Live checks**: §10 (R1–R7), and the UI — the Launch button's hover menu (open delay, dismissal, a
  click while it is open), the Presets tab (placeholders, Save / Cancel, Remove stored key), a refusal
  notice.
- **Display** (§8.3): the pill, the vendor-prefix-less model name, a per-provider ⚡ cache rule (the
  current rule is Anthropic's 5 minutes), the Sessions chip, the CLI field.
- **Test key** and **Fetch limits** (OpenRouter's endpoints data as a tier between the env and the
  template, cached in `model-limits.json`). A key test could also read the balance: OpenRouter refuses
  a request the balance cannot pay in full (§4.1), $0.04–1.92 per request for the shipped templates.
- **`launch-settings` cleanup**: delete the files no live or resumable session references.
- **Copy ▸ Launch CLI** of a DORMANT preset session builds the plain command (`AgentTabOverlay`'s
  fallback); a live session's copy reads the real command line from its process.
- A Deactivated (parked) tab's never-started connection holds the key in its env block until it wakes;
  the key is read at park time, not at wake.
- **Codex presets** (`agent: "codex"`, the providers' `openaiBaseUrl`).
- **Adopt a hand-typed `mimo`**: infer the preset from the PEB `ANTHROPIC_BASE_URL` and the transcript's
  model.
- An ad-hoc `@openrouter:<model>` target, and an import of `openrouter-profiles.json`.
- `defaultLaunchTarget` (D9).

## 13. Code map

| File | What |
|---|---|
| `AgentMaster/LaunchPresets.h` | The PURE core (header-only): field keys + the field table, providers, templates + seeding, `launch-presets.json` (de)serialization, the tiers (`ResolvePresetField`, `ResolveProviderBaseUrl`), the snapshot (`LaunchArgs`, `SnapshotPresetLaunch`, `SnapshotModelLaunch`, `EffectiveLaunchValues`, JSON), the refusal (`PresetLaunchRefusal`), the compile (`CompileClaudeLaunch`, `ComposePresetExtraBody`), the preview, `MaskSecret`, the menu entries + the /handover hint spec. |
| `AgentMaster/LaunchPresetsOs.h` | The OS half (TerminalApp only): the two env lookups (Agentmaster's list; Windows = process env, then the registry), Credential Manager (`Read/Write/DeletePresetProviderKey`), the stamp-cached config loader, `LaunchMenuEntries` / `LaunchMenuSpec`, `LaunchArgsForTarget`, `Load/StoreLaunchArgs`, `ResolveLaunchKey`, `CompileLaunchForSpawn`. |
| `AgentMaster/ClaudeSpawn.{h,cpp}` | `ClaudeSpawnSpec::authTokenEnv` / `presetId`; `BuildClaudeCommandline`'s `extraArgs`; `RenderLaunchExtraArgs`; `BuildLaunchSettingsJson`; `MaterializeLaunchSettingsFile`; `ApplyCompiledLaunch`; the `launch` parameter of `BuildClaudeSpawn` / `BuildClaudeRestartSpec`; `BuildPwshHostedCommandline`'s token scoping; `ParseHandoverArgsHints`' preset rule (D13). |
| `AgentMaster/Persistence.{h,cpp}` | `LaunchPresetsPath` / `LoadLaunchPresetsText` / `SaveLaunchPresetsText` (atomic). |
| `TerminalPage.AgentSessions.cpp` (+ `.h`) | `_ResolveLaunchForSpawn` (the ONE launch-target decision), `_StoredLaunchArgsFollowingChain`, `_PromptPresetLaunchError`, and the seams: `_LaunchClaudeSession` (+ `inheritLaunchFromId`), the /handover successors, `_RestartTabIntoFreshSession`, `_RestartManagedSession`, Deactivate. |
| `AgentModelMenu.h` | The preset group (separator + tooltips) in `AgentFillModelPickItems`. |
| `AgentManagerContent.Settings.cpp` (+ `.h`, `.cpp`) | The Presets tab (§8.1). |
| `AgentLaunchBar.cpp` (+ `.h`) | The Launch button's hover menu and `_OnLaunch(target)` — moved here from `AgentManagerContent.Launch.cpp` on 2026-10-06 (the shared launch bar the Agent Manager page and the side panel's `+` launcher both run; SIDE_PANEL.md §2e). |
| `AgentManagerContent.Tree.cpp`, `TerminalPage.AgentSessionsPage.cpp`, `TerminalPage.cpp` | The menu sites switched from `ParseLaunchModels` to `LaunchMenuEntries`. |
| `AgentMaster/tests/tests_presets.cpp` | `TestLaunchPresets`: seeding (and an upgrade that adds templates), the JSON, the tiers, the snapshot, the compile vs the reference block, every OpenRouter template vs its launcher profile (ordered JSON body, models, limits, effort), effort (`none`, the extra-env override), `top_k` / `require_parameters`, the refusals, the reserved names / forbidden flags, the mask, the menus, the /handover hint's preset rule, and the launch settings file on disk (valid JSON, no key, reuse, the rewrite of a broken file, no temp left). |
| `AgentMaster/tests/tests_spawn_sched.cpp` | The pwsh host script's token scoping (carrier → `ANTHROPIC_AUTH_TOKEN` → removed in `finally`) and the refusal of an unsafe carrier name. |
