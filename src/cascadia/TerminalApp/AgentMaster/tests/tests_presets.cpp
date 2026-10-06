// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 test harness - PRESETS (doc/agentmaster/HARNESSES.md; AgentMaster/LaunchPresets.h).
//
// TestLaunchPresets pins the pure half of the feature:
//   * the shipped templates seed ONCE (a deleted one stays deleted; an unverified one is seeded hidden);
//   * launch-presets.json round-trips, and a malformed file reads as an empty config;
//   * the value tiers - box > Agentmaster env > Windows env > template - for a placeholder AND a snapshot;
//   * the snapshot freezes tiers 1-2, records only the NAME of a Windows variable (re-read at every
//     launch), never stores a key, and survives a JSON round trip;
//   * the MiMo Pro template compiles to EXACTLY the env block the reference launcher generates
//     (~/.claude/scripts/openrouter.ps1, OPENROUTER_DRY_RUN=1, 2026-10-06), apart from the /model display
//     names (the launcher fetches OpenRouter's names; Agentmaster derives them offline), and with no identity
//     note (the launcher passes one; the user dropped it 2026-10-06), while a typed note still compiles;
//   * a Xiaomi preset gets no OpenRouter routing block; the Claude-login preset compiles to flags only;
//   * refusals (no key / no Base URL / a removed custom provider), reserved env names, forbidden claude
//     flags, the key mask;
//   * the menu entries and the /handover hint spec list visible presets after the launch models.

#include "m5_tests.h"

#include "../LaunchPresets.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace Agentmaster;

namespace
{
    // An env lookup over a fixed map (case-insensitive like Windows). Missing names read as "".
    //   vars - name -> value.
    // Returns the lookup.
    PresetEnvLookup MapEnv(std::map<std::wstring, std::wstring> vars)
    {
        return [vars = std::move(vars)](std::wstring_view name) -> std::wstring {
            for (const auto& [k, v] : vars)
            {
                if (detail::PresetEqualsCi(k, name))
                {
                    return v;
                }
            }
            return {};
        };
    }

    // The value of `name` in a compiled settings env block ("" when absent).
    std::wstring EnvOf(const CompiledClaudeLaunch& c, std::wstring_view name)
    {
        for (const auto& [k, v] : c.settingsEnv)
        {
            if (k == name)
            {
                return v;
            }
        }
        return L"<absent>";
    }

    // A config with the templates seeded, as the OS loader produces on a first run.
    LaunchPresetsConfig SeededConfig()
    {
        LaunchPresetsConfig cfg;
        SeedLaunchPresetTemplates(cfg);
        return cfg;
    }

    // A UTF-8 file's text as UTF-16.
    //   path - the file.
    // Returns the text, or "" when the file is missing, empty or not UTF-8 (the caller's CHECK then fails).
    std::wstring ReadUtf8FileText(const std::wstring& path)
    {
        std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (n <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
        return out;
    }
}

void TestLaunchPresets()
{
    std::wprintf(L"\n[TestLaunchPresets]\n");
    const int checksBefore = g_checks;

    // --- seeding: once, deletion sticks, unverified templates start hidden ---
    {
        LaunchPresetsConfig cfg;
        CHECK(SeedLaunchPresetTemplates(cfg), "presets: a first seed adds the templates");
        CHECK(cfg.presets.size() == LaunchPresetTemplates().size(), "presets: one preset per template");
        CHECK(cfg.presets.front().id == L"mimo-pro.openrouter" && cfg.presets.front().templateId == L"mimo-pro.openrouter", "presets: a seeded preset keeps the template id and links to it");
        CHECK(!cfg.presets[0].hidden && !cfg.presets[1].hidden, "presets: the tested OpenRouter templates are visible");
        CHECK(cfg.presets[2].hidden && cfg.presets[3].hidden, "presets: the unverified Xiaomi templates start hidden");
        CHECK(!SeedLaunchPresetTemplates(cfg), "presets: a second seed changes nothing");
        cfg.presets.erase(cfg.presets.begin()); // the user deletes MiMo Pro
        CHECK(!SeedLaunchPresetTemplates(cfg) && !FindLaunchPreset(cfg, L"mimo-pro.openrouter"), "presets: a deleted template preset is never re-seeded");
    }

    // --- launch-presets.json round trip + tolerance ---
    {
        auto cfg = SeededConfig();
        cfg.presets[1].hidden = true;
        cfg.presets[0].fields[kPresetFieldName] = L"Pro on OR";
        cfg.presets[0].fields[kPresetFieldSmallModel] = L"xiaomi/mimo-v2.6-flash";
        LaunchProviderEntry custom;
        custom.id = L"c-12345678";
        custom.custom = true;
        custom.name = L"GLM";
        custom.envPrefix = L"GLM";
        custom.anthropicBaseUrl = L"https://example.invalid/anthropic";
        cfg.providers.push_back(custom);
        LaunchProviderEntry orOverride;
        orOverride.id = L"openrouter";
        orOverride.anthropicBaseUrl = L"http://127.0.0.1:9999";
        cfg.providers.push_back(orOverride);
        const auto back = LaunchPresetsConfigFromJson(LaunchPresetsConfigToJson(cfg));
        CHECK(back.presets.size() == cfg.presets.size(), "presets json: preset count survives");
        CHECK(back.presets[0].fields.at(kPresetFieldName) == L"Pro on OR" && back.presets[0].fields.at(kPresetFieldSmallModel) == L"xiaomi/mimo-v2.6-flash", "presets json: sparse fields survive");
        CHECK(back.presets[1].hidden && !back.presets[0].hidden, "presets json: the hidden flag survives");
        CHECK(back.seededTemplates == cfg.seededTemplates, "presets json: the seeded record survives");
        CHECK(back.providers.size() == 2 && back.providers[0].custom && back.providers[0].envPrefix == L"GLM", "presets json: a custom provider survives");
        CHECK(back.providers[1].anthropicBaseUrl == L"http://127.0.0.1:9999", "presets json: a built-in override survives");
        CHECK(LaunchPresetsConfigFromJson(L"not json").presets.empty(), "presets json: malformed text reads as an empty config");
        CHECK(LaunchPresetsConfigFromJson(L"").presets.empty(), "presets json: an empty file reads as an empty config");
        CHECK(LaunchPresetsConfigFromJson(L"{\"presets\":[{\"id\":\"a\"},{\"id\":\"a\"},{\"x\":1}]}").presets.size() == 1, "presets json: duplicate ids and id-less entries are dropped");
    }

    // --- tiers: box > Agentmaster env > Windows env > template ---
    {
        const auto cfg = SeededConfig();
        LaunchPreset p = *FindLaunchPreset(cfg, L"mimo-pro.openrouter");
        const auto none = MapEnv({});
        auto v = ResolvePresetField(kPresetFieldSmallModel, p, cfg, none, none, true);
        CHECK(v.value == L"xiaomi/mimo-v2.6-pro" && v.source == PresetValueSource::Template, "tiers: an empty box resolves to the template");
        CHECK(v.envName == L"OPENROUTER_SMALL_MODEL", "tiers: the env name is <PREFIX>_<SUFFIX>");
        v = ResolvePresetField(kPresetFieldSmallModel, p, cfg, none, MapEnv({ { L"OPENROUTER_SMALL_MODEL", L"win/model" } }), true);
        CHECK(v.value == L"win/model" && v.source == PresetValueSource::WindowsEnv, "tiers: a Windows variable beats the template");
        v = ResolvePresetField(kPresetFieldSmallModel, p, cfg, MapEnv({ { L"openrouter_small_model", L"am/model" } }), MapEnv({ { L"OPENROUTER_SMALL_MODEL", L"win/model" } }), true);
        CHECK(v.value == L"am/model" && v.source == PresetValueSource::AgentmasterEnv, "tiers: Agentmaster's env beats Windows (names case-insensitive)");
        p.fields[kPresetFieldSmallModel] = L"  box/model  ";
        v = ResolvePresetField(kPresetFieldSmallModel, p, cfg, MapEnv({ { L"OPENROUTER_SMALL_MODEL", L"am/model" } }), none, true);
        CHECK(v.value == L"box/model" && v.source == PresetValueSource::Box, "tiers: the box beats everything, trimmed");
        v = ResolvePresetField(kPresetFieldSmallModel, p, cfg, MapEnv({ { L"OPENROUTER_SMALL_MODEL", L"am/model" } }), none, false);
        CHECK(v.value == L"am/model", "tiers: the placeholder (includeBox=false) shows what applies below the box");
        CHECK(PresetValueSourceLabel(v) == L"OPENROUTER_SMALL_MODEL (Agentmaster env)", "tiers: the source label names the variable");
        v = ResolvePresetField(kPresetFieldModel, p, cfg, MapEnv({ { L"OPENROUTER_MODEL", L"x" } }), none, true);
        CHECK(v.envName.empty() && v.value == L"xiaomi/mimo-v2.6-pro", "tiers: the main model has no env tier");

        const auto prov = ResolveLaunchProvider(cfg, L"openrouter");
        auto url = ResolveProviderBaseUrl(prov, none, none, true);
        CHECK(url.value == L"https://openrouter.ai/api" && url.source == PresetValueSource::ProviderDefault, "tiers: the Base URL falls back to the provider default");
        url = ResolveProviderBaseUrl(prov, none, MapEnv({ { L"OPENROUTER_BASE_URL", L"http://proxy" } }), true);
        CHECK(url.value == L"http://proxy" && url.source == PresetValueSource::WindowsEnv, "tiers: OPENROUTER_BASE_URL beats the default");
        CHECK(!ResolveLaunchProvider(cfg, kAnthropicProviderId).found && !ResolveLaunchProvider(cfg, L"").found, "providers: the Claude login is no provider");
        CHECK(PresetDisplayName(p) == L"MiMo Pro \x00B7 OpenRouter", "presets: the display name comes from the template");
        CHECK(PresetLaunchRefusal(p, cfg).empty(), "refusal: a built-in provider launches");
        p.fields[kPresetFieldProvider] = kAnthropicProviderId;
        CHECK(PresetProviderId(p).empty(), "presets: an explicit Claude-login box overrides the template's provider");
        CHECK(PresetLaunchRefusal(p, cfg).empty(), "refusal: the Claude login launches");

        // A custom provider removed in Settings: the preset refuses, it never falls back to the Claude login.
        auto withCustom = cfg;
        LaunchProviderEntry custom;
        custom.id = L"c-87654321";
        custom.custom = true;
        custom.name = L"GLM";
        custom.envPrefix = L"GLM";
        withCustom.providers.push_back(custom);
        LaunchPreset glm;
        glm.id = L"p-11111111";
        glm.fields[kPresetFieldName] = L"My GLM";
        glm.fields[kPresetFieldProvider] = L"c-87654321";
        CHECK(PresetLaunchRefusal(glm, withCustom).empty(), "refusal: an existing custom provider launches");
        const auto refusal = PresetLaunchRefusal(glm, cfg); // the same preset after the provider's removal
        CHECK(refusal.find(L"\"My GLM\"") != std::wstring::npos && refusal.find(L"\"c-87654321\"") != std::wstring::npos, "refusal: a removed custom provider refuses, naming the preset and the provider");
        withCustom.providers.back().custom = false; // an entry that is not a custom provider and matches no built-in
        CHECK(!PresetLaunchRefusal(glm, withCustom).empty(), "refusal: an unknown non-custom provider id refuses too");
    }

    // --- the snapshot: tiers 1-2 frozen, Windows names recorded, never a key ---
    LaunchArgs snap;
    {
        auto cfg = SeededConfig();
        LaunchPreset p = *FindLaunchPreset(cfg, L"mimo-pro.openrouter");
        p.fields[kPresetFieldTopP] = L"0.9";
        snap = SnapshotPresetLaunch(p, cfg, MapEnv({ { L"OPENROUTER_CONTEXT", L"500000" }, { L"OPENROUTER_API_KEY", L"sk-or-v1-secret-value-1234" } }));
        CHECK(snap.presetId == L"mimo-pro.openrouter" && snap.providerId == L"openrouter" && snap.routing, "snapshot: provenance + provider frozen");
        CHECK(snap.set.at(kPresetFieldTopP) == L"0.9", "snapshot: a typed box is frozen into set");
        CHECK(snap.set.at(kPresetFieldContext) == L"500000", "snapshot: an Agentmaster env value is frozen into set");
        CHECK(snap.env.at(kPresetFieldSmallModel) == L"OPENROUTER_SMALL_MODEL" && snap.base.at(kPresetFieldSmallModel) == L"xiaomi/mimo-v2.6-pro", "snapshot: a Windows-tier field keeps its NAME and the template value as base");
        CHECK(snap.env.at(kPresetFieldBaseUrl) == L"OPENROUTER_BASE_URL" && snap.base.at(kPresetFieldBaseUrl) == L"https://openrouter.ai/api", "snapshot: the Base URL is env-tier + default");
        CHECK(snap.keyCredential == L"openrouter" && snap.keyEnv == L"OPENROUTER_API_KEY", "snapshot: the key is a reference");
        const auto json = LaunchArgsToJson(snap);
        CHECK(json.find(L"secret") == std::wstring::npos, "snapshot: the key value never reaches the stored JSON");
        const auto back = LaunchArgsFromJson(json);
        CHECK(back && back->set == snap.set && back->env == snap.env && back->base == snap.base && back->keyEnv == snap.keyEnv && back->presetName == snap.presetName, "snapshot: JSON round trip");
        CHECK(!LaunchArgsFromJson(L"{\"v\":2}"), "snapshot: a newer schema is refused");
        CHECK(!LaunchArgsFromJson(L"{\"v\":1,\"agent\":\"codex\"}"), "snapshot: a Codex snapshot is not launchable as Claude");
        CHECK(!LaunchArgsFromJson(L"") && !LaunchArgsFromJson(L"[1]"), "snapshot: empty / non-object text is refused");

        // The preset changes AFTER the snapshot: the session does not follow.
        cfg.presets.front().fields[kPresetFieldModel] = L"other/model";
        const auto values = EffectiveLaunchValues(snap, MapEnv({}));
        CHECK(values.at(kPresetFieldModel) == L"xiaomi/mimo-v2.6-pro", "snapshot: a later preset edit never changes the session");
        // A Windows variable set AFTER the snapshot reaches the next launch.
        const auto later = EffectiveLaunchValues(snap, MapEnv({ { L"OPENROUTER_SMALL_MODEL", L"late/model" }, { L"OPENROUTER_TOPP", L"x" } }));
        CHECK(later.at(kPresetFieldSmallModel) == L"late/model", "snapshot: a Windows variable is re-read at every launch");
        CHECK(later.at(kPresetFieldTopP) == L"0.9", "snapshot: a frozen value beats any variable");
        const auto plain = SnapshotModelLaunch(L"  fable ");
        CHECK(plain.set.at(kPresetFieldModel) == L"fable" && plain.providerId.empty() && plain.presetId.empty(), "snapshot: a plain model pick keeps just the model");
    }

    // --- compile: the MiMo Pro template == the reference launcher's env block ---
    {
        const auto cfg = SeededConfig();
        const auto a = SnapshotPresetLaunch(*FindLaunchPreset(cfg, L"mimo-pro.openrouter"), cfg, MapEnv({}));
        const auto c = CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"sk-or-v1-abcdefabcdef9876");
        CHECK(c.error.empty() && c.sessionSettings, "compile: a provider preset writes a settings file");
        CHECK(EnvOf(c, L"ANTHROPIC_BASE_URL") == L"https://openrouter.ai/api", "compile: base URL");
        CHECK(EnvOf(c, L"ANTHROPIC_API_KEY").empty(), "compile: ANTHROPIC_API_KEY is set EMPTY (no custom-key modal)");
        for (const auto* k : { L"ANTHROPIC_MODEL", L"ANTHROPIC_DEFAULT_FABLE_MODEL", L"ANTHROPIC_DEFAULT_OPUS_MODEL", L"ANTHROPIC_DEFAULT_SONNET_MODEL", L"ANTHROPIC_DEFAULT_HAIKU_MODEL", L"CLAUDE_CODE_SUBAGENT_MODEL" })
        {
            CHECK(EnvOf(c, k) == L"xiaomi/mimo-v2.6-pro", "compile: every tier is pinned to the main model");
        }
        CHECK(EnvOf(c, L"ANTHROPIC_DEFAULT_OPUS_MODEL_NAME") == L"mimo-v2.6-pro (OpenRouter)", "compile: display names drop the vendor prefix");
        CHECK(EnvOf(c, L"CLAUDE_CODE_MAX_OUTPUT_TOKENS") == L"131072" && EnvOf(c, L"CLAUDE_CODE_MAX_CONTEXT_TOKENS") == L"1048576", "compile: limits");
        CHECK(EnvOf(c, L"ANTHROPIC_CUSTOM_MODEL_OPTION") == L"xiaomi/mimo-v2.6-flash", "compile: the /model switch entry");
        CHECK(EnvOf(c, L"CLAUDE_CODE_EXTRA_BODY") == L"{\"provider\":{\"order\":[\"gmicloud\",\"novita\",\"deepinfra\"],\"quantizations\":[\"bf16\",\"fp8\"]},\"temperature\":1,\"top_p\":0.95}",
              "compile: the request body is the launcher's (provider routing + sampling)");
        // No template ships an identity note (the user, 2026-10-06: "not necessary"), unlike the reference launcher.
        CHECK(c.appendSystemPrompt.empty(), "compile: the templates append no identity note");
        CHECK(c.model == L"xiaomi/mimo-v2.6-pro", "compile: --model carries the main model");
        CHECK(c.authTokenEnv == kPresetTokenEnvName, "compile: the key rides the scoped token variable");
        bool tokenInProcess = false, presetStamp = false;
        for (const auto& [k, v] : c.processEnv)
        {
            tokenInProcess |= (k == kPresetTokenEnvName && v == L"sk-or-v1-abcdefabcdef9876");
            presetStamp |= (k == kPresetIdEnvName && v == L"mimo-pro.openrouter");
        }
        CHECK(tokenInProcess && presetStamp, "compile: process env = the token + the AM_PRESET stamp");
        for (const auto& [k, v] : c.settingsEnv)
        {
            CHECK(v.find(L"abcdefabcdef") == std::wstring::npos, "compile: the key never reaches the settings file");
        }
        const auto preview = RenderPresetLaunchPreview(c, MaskSecret(L"sk-or-v1-abcdefabcdef9876"));
        CHECK(preview.find(L"abcdefabcdef") == std::wstring::npos && preview.find(L"sk-or-v1-\x2026" L"9876") != std::wstring::npos, "compile: the preview shows only the masked key");

        // The refusals.
        CHECK(!CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"  ").error.empty(), "compile: no key = refuse");
        auto noUrl = a;
        noUrl.base.erase(kPresetFieldBaseUrl);
        CHECK(!CompileClaudeLaunch(noUrl, EffectiveLaunchValues(noUrl, MapEnv({})), L"k").error.empty(), "compile: no Base URL = refuse");

        // The Identity note box stays optional: a note the user TYPES is appended with its placeholders filled,
        // and "none" still means no note.
        auto noted = *FindLaunchPreset(cfg, L"mimo-pro.openrouter");
        noted.fields[kPresetFieldIdentityNote] = L"Runs on {model} via {provider}{switch}.";
        const auto na = SnapshotPresetLaunch(noted, cfg, MapEnv({}));
        CHECK(CompileClaudeLaunch(na, EffectiveLaunchValues(na, MapEnv({})), L"k-0123456789abcdef").appendSystemPrompt ==
                  L"Runs on xiaomi/mimo-v2.6-pro via OpenRouter, unless the user switched to xiaomi/mimo-v2.6-flash with /model.",
              "compile: a typed identity note is appended, its placeholders filled");
        noted.fields[kPresetFieldIdentityNote] = L"None";
        const auto xa = SnapshotPresetLaunch(noted, cfg, MapEnv({}));
        CHECK(CompileClaudeLaunch(xa, EffectiveLaunchValues(xa, MapEnv({})), L"k-0123456789abcdef").appendSystemPrompt.empty(),
              "compile: a typed \"none\" appends no note");
    }

    // --- Xiaomi: no routing block; Claude login: flags only ---
    {
        const auto cfg = SeededConfig();
        const auto a = SnapshotPresetLaunch(*FindLaunchPreset(cfg, L"mimo-pro.xiaomi"), cfg, MapEnv({}));
        CHECK(!a.routing && a.keyEnv == L"MIMO_API_KEY", "xiaomi: no routing, MIMO_API_KEY");
        const auto c = CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"tp-xxxxxxxxxxxxxxxx");
        CHECK(EnvOf(c, L"CLAUDE_CODE_EXTRA_BODY") == L"{\"temperature\":1,\"top_p\":0.95}", "xiaomi: the body carries sampling only (no OpenRouter provider block)");
        CHECK(EnvOf(c, L"ANTHROPIC_BASE_URL") == L"https://api.xiaomimimo.com/anthropic", "xiaomi: the pay-as-you-go Anthropic endpoint");

        LaunchPreset login;
        login.id = L"p-login";
        login.fields[kPresetFieldName] = L"Opus max";
        login.fields[kPresetFieldModel] = L"opus";
        login.fields[kPresetFieldEffort] = L"MAX";
        login.fields[kPresetFieldTemperature] = L"0.5"; // provider-only: never compiled on the Claude login
        const auto la = SnapshotPresetLaunch(login, cfg, MapEnv({}));
        const auto lc = CompileClaudeLaunch(la, EffectiveLaunchValues(la, MapEnv({})), L"");
        CHECK(lc.error.empty() && !lc.sessionSettings && lc.model == L"opus" && lc.effort == L"max", "login: model + effort ride the command line, no settings file");
        CHECK(lc.authTokenEnv.empty() && la.set.find(kPresetFieldTemperature) == la.set.end(), "login: no token, provider-only fields skipped");
        login.fields[kPresetFieldMaxOutput] = L"64000";
        const auto la2 = SnapshotPresetLaunch(login, cfg, MapEnv({}));
        CHECK(CompileClaudeLaunch(la2, EffectiveLaunchValues(la2, MapEnv({})), L"").sessionSettings, "login: an env value needs the settings file");
    }

    // --- validation: extra env, extra args, numbers ---
    {
        const auto cfg = SeededConfig();
        LaunchPreset p = *FindLaunchPreset(cfg, L"mimo-pro.openrouter");
        p.fields[kPresetFieldExtraEnv] = L"API_TIMEOUT_MS=900000\nCCMGR_SESSION_ID=x\nAM_SESSION=y\nnot an assignment\nANTHROPIC_BASE_URL=http://override";
        p.fields[kPresetFieldExtraArgs] = L"--autocompact 600000";
        p.fields[kPresetFieldContext] = L"1,048,576";
        p.fields[kPresetFieldExtraBody] = L"{\"provider\":{\"allow_fallbacks\":false},\"top_p\":0.123456789}";
        const auto a = SnapshotPresetLaunch(p, cfg, MapEnv({}));
        const auto c = CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"k-123456789012");
        CHECK(EnvOf(c, L"API_TIMEOUT_MS") == L"900000", "validate: extra env reaches the settings file");
        CHECK(EnvOf(c, L"CCMGR_SESSION_ID") == L"<absent>" && EnvOf(c, L"AM_SESSION") == L"<absent>", "validate: reserved names are refused");
        CHECK(EnvOf(c, L"ANTHROPIC_BASE_URL") == L"http://override", "validate: extra env overrides a generated key");
        CHECK(EnvOf(c, L"CLAUDE_CODE_MAX_CONTEXT_TOKENS") == L"<absent>", "validate: a non-integer limit is dropped");
        CHECK(c.extraArgs == L"--autocompact 600000", "validate: allowed extra arguments pass");
        CHECK(EnvOf(c, L"CLAUDE_CODE_EXTRA_BODY") == L"{\"provider\":{\"order\":[\"gmicloud\",\"novita\",\"deepinfra\"],\"quantizations\":[\"bf16\",\"fp8\"],\"allow_fallbacks\":false},\"temperature\":1,\"top_p\":0.123456789}",
              "validate: the extra body deep-merges (objects merge, values replace) with no precision loss");
        CHECK(c.warnings.size() >= 4, "validate: every dropped value is reported");
        CHECK(PresetForbiddenArgument(L"--settings x").find(L"--settings") == 0, "validate: --settings is Agentmaster's");
        CHECK(!PresetForbiddenArgument(L"--model=opus").empty() && !PresetForbiddenArgument(L"  -p  hi").empty(), "validate: --model= and print mode are refused");
        CHECK(PresetForbiddenArgument(L"--permission-mode plan --autocompact 600000").empty(), "validate: ordinary flags pass");
    }

    // --- menus, tokens, small helpers ---
    {
        auto cfg = SeededConfig();
        const auto entries = LaunchMenuEntriesFrom({ { L"Opus", L"opus" } }, cfg);
        CHECK(entries.size() == 3 && entries[0].second == L"opus", "menus: launch models first");
        CHECK(entries[1].second == L"@mimo-pro.openrouter" && entries[2].second == L"@mimo-flash.openrouter", "menus: then the VISIBLE presets as @ tokens");
        cfg.presets[0].fields[kPresetFieldName] = L"Pro; with | bars";
        const auto spec = LaunchMenuSpecFrom(L"Opus | opus", cfg);
        CHECK(spec.find(L"Pro  with   bars | @mimo-pro.openrouter") != std::wstring::npos, "menus: the hint spec sanitizes ; and |");
        CHECK(IsPresetToken(L"@x") && !IsPresetToken(L"@") && !IsPresetToken(L"opus") && PresetIdFromToken(L"@abc") == L"abc" && PresetToken(L"abc") == L"@abc", "tokens");
        CHECK(MaskSecret(L"sk-or-v1-0123456789abcdef") == L"sk-or-v1-\x2026" L"cdef", "mask: kind prefix + last 4");
        CHECK(MaskSecret(L"short") == L"\x2022\x2022\x2022" && MaskSecret(L"").empty(), "mask: short and empty keys");
        CHECK(MaskSecret(L"abcdefghijklmnopqrstu") == L"\x2026rstu", "mask: no dash = last 4 only");
        CHECK(SanitizeLaunchEnvPrefix(L" my-glm 2_ ") == L"MY_GLM_2" && SanitizeLaunchEnvPrefix(L"9x").empty() == false, "prefix: sanitized");
        CHECK(SanitizeLaunchEnvPrefix(L"9x") == L"X", "prefix: no leading digit");
        CHECK(PresetModelDisplayName(L"xiaomi/mimo-v2.6-pro", L"OpenRouter") == L"mimo-v2.6-pro (OpenRouter)" && PresetModelDisplayName(L"opus", L"") == L"opus", "display name");
        std::vector<std::wstring> taken;
        for (int i = 0; i < 50; ++i)
        {
            const auto id = MakeLaunchPresetId(L'p', 42, [&](const std::wstring& x) { return std::find(taken.begin(), taken.end(), x) != taken.end(); });
            CHECK(id.size() == 10 && id.rfind(L"p-", 0) == 0 && std::find(taken.begin(), taken.end(), id) == taken.end(), "ids: unique under collisions");
            taken.push_back(id);
        }
        CHECK(RenderPresetIdentityNote(L"{model} via {provider}{switch}.", L"m", L"P", L"") == L"m via P.", "note: no switch clause without a switch model");
        CHECK(IsValidPresetEffort(L"xHigh") && !IsValidPresetEffort(L"ultra"), "effort levels");
    }

    // --- the launch settings file (ClaudeSpawn.cpp, HARNESSES.md §6.1): valid JSON with the hooks + the env
    // block, no key, content-addressed, reused when intact, REWRITTEN when it does not match its name ---
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        // A private scratch folder: the temp dir + a fixed prefix + this process's id (never a user path).
        const fs::path dir = fs::temp_directory_path(ec) / (L"am-presets-test-" + std::to_wstring(::GetCurrentProcessId()));
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const auto cfg = SeededConfig();
        const auto a = SnapshotPresetLaunch(*FindLaunchPreset(cfg, L"mimo-pro.openrouter"), cfg, MapEnv({}));
        const auto c = CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"sk-or-v1-abcdefabcdef9876");
        const AppSettings settings{};
        const std::wstring forwarder = (dir / L"agentmaster-hook.ps1").wstring();
        const auto path = MaterializeLaunchSettingsFile(dir.wstring(), settings, L"", forwarder, c);
        CHECK(!path.empty() && fs::exists(path), "launch file: written");
        CHECK(path.find(L"\\launch-settings\\mimo-pro.openrouter-") != std::wstring::npos && path.size() > 5 && path.substr(path.size() - 5) == L".json",
              "launch file: <profile>\\launch-settings\\<preset>-<hash>.json");
        const auto text = ReadUtf8FileText(path);
        const auto parsed = json::Parse(text);
        CHECK(parsed && parsed->type == json::Value::Type::Obj, "launch file: valid JSON (the env block spliced into the hooks document)");
        if (parsed && parsed->type == json::Value::Type::Obj)
        {
            CHECK(parsed->Find(L"hooks") != nullptr, "launch file: carries the hooks (one --settings file per session)");
            CHECK(parsed->StrAt(L"model") == L"xiaomi/mimo-v2.6-pro", "launch file: model = the preset's main model");
            const auto* env = parsed->Find(L"env");
            CHECK(env && env->StrAt(L"ANTHROPIC_BASE_URL") == L"https://openrouter.ai/api" && env->StrAt(L"ANTHROPIC_DEFAULT_OPUS_MODEL") == L"xiaomi/mimo-v2.6-pro",
                  "launch file: the compiled env block");
        }
        CHECK(text.find(L"abcdefabcdef9876") == std::wstring::npos, "launch file: the key never reaches the file");
        CHECK(MaterializeLaunchSettingsFile(dir.wstring(), settings, L"", forwarder, c) == path && ReadUtf8FileText(path) == text,
              "launch file: an identical launch reuses the same file");
        {
            std::ofstream broken(fs::path{ path }, std::ios::binary | std::ios::trunc);
            broken << "{\"hooks\": {"; // a write that died half-way (before the writer was atomic), or a stray edit
        }
        CHECK(MaterializeLaunchSettingsFile(dir.wstring(), settings, L"", forwarder, c) == path && ReadUtf8FileText(path) == text,
              "launch file: a file that does not match its name is REWRITTEN, never reused");
        CHECK(!fs::exists(path + L".am-tmp"), "launch file: the atomic write leaves no temp file");
        auto edited = cfg;
        edited.presets.front().fields[kPresetFieldTopP] = L"0.9";
        const auto a2 = SnapshotPresetLaunch(*FindLaunchPreset(edited, L"mimo-pro.openrouter"), edited, MapEnv({}));
        const auto other = MaterializeLaunchSettingsFile(dir.wstring(), settings, L"", forwarder, CompileClaudeLaunch(a2, EffectiveLaunchValues(a2, MapEnv({})), L"k-0123456789abcdef"));
        CHECK(!other.empty() && other != path && ReadUtf8FileText(path) == text, "launch file: another configuration gets another file; the first is untouched");
        fs::remove_all(dir, ec);
    }

    // The section's own count, so a run log shows these checks really ran (the other sections print only failures).
    std::wprintf(L"  %d checks\n", g_checks - checksBefore);
}
