# 14 · Presets: Claude Code on other models and providers (PRE)

> Conventions and fixtures: [README.md](README.md). Spec: HARNESSES.md §4–§8 (as built), §6.4 (refusals), §7
> (stickiness); CLAUDE.md *PRESETS*. Needs the mock and **F-PRE**: a CUSTOM provider preset whose Base URL is the
> mock (`http://127.0.0.1:8787`) with a model name — the mock then logs the base URL, the token header and the model
> each preset launch really used.

**What shipped (0.7.9).** Named presets appear after a separator in every Open New Session Here / Fork session menu
and the Launch button's hover menu. The list starts EMPTY: until a preset exists every launch menu offers **Add
presets…** (a help dialog pointing at Settings → Presets → TEMPLATES). Each field resolves box > Agentmaster env >
Windows env > template default (shown as gray placeholders); keys live in Windows Credential Manager and reach only
that one `claude`, as `ANTHROPIC_AUTH_TOKEN`; a session keeps its preset through resume, restart, fork and /handover
(its `launchArgs` snapshot); a preset that cannot launch is refused, never downgraded to the Claude login.

## Lifecycle model

```text
 Settings → Presets → TEMPLATES ─Add─► a preset (linked to its template) ─fields + key (Credential Manager)─► visible in menus
 a pick in a menu ─► resolve (box > Agentmaster env > Windows env > template) ─► refuse?  ── yes ─► "This preset can't start" (nothing starts)
                                                                             └─ no ─► content-addressed --settings file (hooks + env) ─► claude
 the session's launchArgs snapshot ─► reused by resume · window restore · Deactivate wake · Restart · Default fork · /handover · /clear successor
```

---

### PRE-01 · The empty list offers "Add presets…"

> **Since** 0.7.9 · **P1** · **Spec** HARNESSES.md §8.2

**Phases**
1. **Menus.** *Do:* a fresh profile; open the tab menu's Fork session ▸, a board card's Open New Session Here ▸, the
   Launch button's hover menu. *Expect:* `Default`, `Specify…`, the launch models (Fable / Opus / Sonnet), then **Add
   presets…** in the presets' place.
2. **Help.** *Do:* pick Add presets…. *Expect:* an OK dialog "Add presets" with the steps (Home → ⚙ → Presets →
   TEMPLATES → Add / Add all tested templates → Save) and the OpenRouter key note; nothing launches.

**Edge cases**
- **E1 Every preset hidden** ("Show in the launch menus" off) → no Add presets… item.
- **E2 The WT tab menu with an empty launch-model list** → keeps its plain New Session Here (no Add presets…).

---

### PRE-02 · Add a preset and launch on it

> **Since** 0.7.9 · **P0** · **Needs** F-PRE · **Spec** HARNESSES.md §5–§6, §8.1

**Phases**
1. **Add.** *Do:* Settings → Presets → add a CUSTOM provider "Mock" with Base URL = the mock and a model `mock-model-1`;
   a preset on it with a key `test-key`; Save. *Expect:* the key stored in Credential Manager (`Agentmaster:provider:<id>`),
   never in `launch-presets.json`; [LOG] `[nav] presets-save presets=1 keys-stored=1 …`.
2. **Launch.** *Do:* a board card → Open New Session Here ▸ **Mock preset**; first prompt `[[mock reply "hi"]]`.
   *Expect:* the session starts; [MOCK] its requests arrive at the preset's URL, `Authorization: Bearer test-key`, model
   `mock-model-1`; [LOG] `[spawn] <sid> "<title>" cwd=… preset=<id> model=mock-model-1` (never the key); [OS] the claude
   process's command line carries `--settings <profile>\launch-settings\<preset>-<hash>.json`, and that file holds the
   hooks + the env (no key in it — verify).
3. **Effort.** *Expect:* the preset's effort rides the settings file as `CLAUDE_CODE_EFFORT_LEVEL`; [MOCK] the request's
   effort matches.

**Edge cases**
- **E1 Placeholders** → an empty box shows the inherited value (Agentmaster env, Windows env, or the template) in gray.
- **E2 An ANTHROPIC_API_KEY in the environment** → never used for the preset (the token is `ANTHROPIC_AUTH_TOKEN`); no
  "use this API key?" modal.
- **E3 Two identical launches** → one shared settings file (content-addressed).

---

### PRE-03 · A preset that cannot launch is refused

> **Since** 0.7.9 · **P0** · **Needs** F-PRE · **Spec** HARNESSES.md §6.4

**Phases** (each: pick the preset; expect "This preset can't start" naming the reason; NOTHING starts; [LOG]
`[launch-blocked] target @<id>: <reason>`)
1. **No key** for its provider.
2. **No Base URL** (a custom provider with an empty box and no `<PREFIX>_BASE_URL`).
3. **RunPod with no model.**
4. **The provider deleted** after the preset was made.

**Edge cases**
- **E1 A window restore of N keyless sessions** → ONE notice, not N.
- **E2 Restart session of such a session** → `[restart-blocked] …`; the old connection stays.
- **E3 Deactivate of such a session** → `[deactivate] … blocked - its preset can't launch`; the session keeps running.
- **E4 /handover-here** → the in-place launch refused, then the new-tab path refuses the same way.

---

### PRE-04 · A session keeps its preset

> **Since** 0.7.9 · **P0** · **Needs** F-PRE, F-RS · **Spec** HARNESSES.md §7

**Phases** (a session launched on the Mock preset; after each step [MOCK] shows the preset's URL and model)
1. **Restart session** → the same preset.
2. **Deactivate → wake** → the same preset.
3. **Quit → reopen → enter** → the same preset.
4. **Close → Sessions → Resume here** → the same preset.
5. **Fork ▸ Default** → the fork runs on the SOURCE's preset; **Fork ▸ Opus** → the fork runs on opus.
6. **/clear in the tab** → the successor keeps the preset.

**Edge cases**
- **E1 The preset edited after launch** (another model) → the running session's snapshot keeps the OLD values (values,
  not a reference); a new launch uses the new ones.
- **E2 A Default launch** → no snapshot; the session follows the cog's model.

---

### PRE-05 · /handover's model hint picks a preset only by a bracketed prefix

> **Since** 0.7.9 · **P1** · **Needs** F-PRE, the /handover command, mock (`write HANDOVER-x.md`) · **Spec** HARNESSES.md §8.2; COMMANDS.md §6b

**Phases**
1. `/handover [mock] do x [[mock write HANDOVER-x.md "brief"; reply "done"]]` → the successor runs on the Mock preset;
   [LOG] `[handover] <sid8> successor model from the message hint: @<id>`.
2. `/handover open the PR …` (no brackets) → never a preset (a bare word never picks one).
3. `/handover [router] …` where "router" only occurs inside a preset's name → a successor TITLE, not a preset.
