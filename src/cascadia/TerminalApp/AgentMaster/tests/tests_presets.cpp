// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 test harness - PRESETS (doc/agentmaster/HARNESSES.md; AgentMaster/LaunchPresets.h).
//
// TestLaunchPresets pins the pure half of the feature:
//   * the list starts EMPTY - no template is added by itself (the user's rule, 2026-10-06) - and an empty list
//     offers "Add presets..." (a list of hidden presets does not), with help text that names the way to the
//     Presets tab; one click adds a template as a LINKED, visible "p-" preset, and "Add all tested" adds the five
//     tested ones in order (the ten templates' order is pinned); a file written by release 0.6.8.18 (which added
//     the templates by itself) loses
//     exactly the presets it added that nobody touched, and is never pruned again once re-saved;
//   * launch-presets.json round-trips, and a malformed file reads as an empty config;
//   * the value tiers - box > Agentmaster env > Windows env > template - for a placeholder AND a snapshot;
//   * the snapshot freezes tiers 1-2, records only the NAME of a Windows variable (re-read at every
//     launch), never stores a key, and survives a JSON round trip;
//   * the MiMo Pro template compiles to EXACTLY the env block the reference launcher generates
//     (~/.claude/scripts/openrouter.ps1, OPENROUTER_DRY_RUN=1, 2026-10-06), apart from the /model display
//     names (the launcher fetches OpenRouter's names; Agentmaster derives them offline), and with no identity
//     note (the launcher passes one; the user dropped it 2026-10-06), while a typed note still compiles;
//   * EVERY OpenRouter template (MiMo Pro / Flash, Kimi K3, Qwen3.8, GLM-5.3) compiles to its launcher
//     profile's request body (openrouter-profiles.json, compared as ordered JSON), the launcher's limits, and
//     the effort the profiles were tested with (max, from the user's settings.json - the templates carry it);
//   * effort rides the settings file as CLAUDE_CODE_EFFORT_LEVEL (Claude Code obeys it over --effort) and
//     the flag; "none" unsets a template's level; an Extra env line still wins, with a warning;
//   * top_k and require_parameters: validation, scope (no routing block on Xiaomi), the extra-body merge;
//   * a Xiaomi preset gets no OpenRouter routing block; the Claude-login preset compiles to flags only;
//   * the built-in provider table (OpenRouter, Xiaomi MiMo, NanoGPT, RunPod); NanoGPT's two templates compile to
//     the NORMAL models with the values of NanoGPT's catalog (2026-10-09) on its direct API host; RunPod has no
//     default URL (the refusal shows the endpoint shapes and RUNPOD_BASE_URL), takes RUNPOD_API_KEY, and refuses a
//     preset that names no model, because a vLLM endpoint serves exactly one;
//   * refusals (no key / no Base URL / a removed custom provider / no model on RunPod), reserved env names,
//     forbidden claude flags, the key mask;
//   * the menu entries and the /handover hint spec list visible presets after the launch models, and the
//     hint picks a preset only through a bracketed name prefix - never by a bare word.

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

    // A config holding one preset per shipped template, LINKED to it, with the TEMPLATE's id as its own id and the
    // untested ones hidden: the list release 0.6.8.18 added by itself, and still a valid list (a user keeps those
    // presets once they touch them). The tier, compile, menu and hint checks below run against every template
    // through it, and its ids keep the launch-file name check readable (<preset>-<hash>.json).
    // Returns the config (never throws except std::bad_alloc).
    LaunchPresetsConfig AllTemplatesConfig()
    {
        LaunchPresetsConfig cfg;
        for (const auto& t : LaunchPresetTemplates())
        {
            LaunchPreset p;
            p.id = t.id;
            p.templateId = t.id;
            p.hidden = t.tested.empty();
            cfg.presets.push_back(std::move(p));
        }
        return cfg;
    }

    // Ordered deep equality of two JSON values: the same types, numbers, strings and arrays (in order), and
    // objects with the same members IN THE SAME ORDER. Numbers compare as doubles, so the launcher's "1.0" and
    // Agentmaster's "1" are equal while a different key order is not (a template's body must read like its
    // launcher profile's).
    //   a, b - the values.
    // Returns true when they are the same.
    bool JsonSame(const json::Value& a, const json::Value& b)
    {
        if (a.type != b.type)
        {
            return false;
        }
        switch (a.type)
        {
        case json::Value::Type::Null:
            return true;
        case json::Value::Type::Bool:
            return a.boolean == b.boolean;
        case json::Value::Type::Num:
            return a.num == b.num;
        case json::Value::Type::Str:
            return a.str == b.str;
        case json::Value::Type::Arr:
            if (a.arr.size() != b.arr.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.arr.size(); ++i)
            {
                if (!JsonSame(a.arr[i], b.arr[i]))
                {
                    return false;
                }
            }
            return true;
        case json::Value::Type::Obj:
            if (a.members.size() != b.members.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.members.size(); ++i)
            {
                if (a.members[i].first != b.members[i].first || !JsonSame(a.members[i].second, b.members[i].second))
                {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    // `text` parsed and compared with JsonSame against `reference`.
    //   text      - the compiled JSON (Agentmaster's number format).
    //   reference - the reference JSON (the launcher profile's text, verbatim).
    // Returns false when either does not parse.
    bool JsonTextSame(std::wstring_view text, std::wstring_view reference)
    {
        const auto x = json::Parse(text);
        const auto y = json::Parse(reference);
        return x && y && JsonSame(*x, *y);
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

    // --- the initial list is EMPTY: no template is added by itself (the user's rule, 2026-10-06), and the launch
    // menus then offer "Add presets..." in the presets' place (the item itself is AgentModelMenu.h's) ---
    {
        const auto none = LaunchPresetsConfigFromJson(L"");
        CHECK(none.presets.empty() && none.providers.empty(), "initial: no file = an empty list (no template is added by itself)");
        CHECK(ShouldOfferAddPresets(none), "initial: an empty list offers Add presets...");
        const auto entries = LaunchMenuEntriesFrom({ { L"Opus", L"opus" } }, none);
        CHECK(entries.size() == 1 && entries[0].second == L"opus", "initial: the menu entries are just the launch models");
        CHECK(LaunchMenuSpecFrom(L"Opus | opus", none) == L"Opus | opus", "initial: the /handover hint spec gains nothing");
        LaunchPresetsConfig hiddenOnly;
        LaunchPreset hidden;
        hidden.id = L"p-0000beef";
        hidden.hidden = true;
        hiddenOnly.presets.push_back(hidden);
        CHECK(!ShouldOfferAddPresets(hiddenOnly) && LaunchMenuEntriesFrom({}, hiddenOnly).empty(),
              "initial: a list whose presets are all hidden lists none AND offers no Add presets... (the user has presets)");
        const auto help = AddPresetsHelpText(false);
        CHECK(help.find(L"1. Open the Agent Manager") != std::wstring::npos && help.find(L"2. Click \x2699 (Settings)") != std::wstring::npos &&
                  help.find(L"Presets tab") != std::wstring::npos && help.find(L"TEMPLATES") != std::wstring::npos && help.find(L"4. Click Save.") != std::wstring::npos,
              "help: outside the Agent Manager - open it, Settings, the Presets tab, TEMPLATES, Save (4 steps)");
        const auto helpIn = AddPresetsHelpText(true);
        CHECK(helpIn.find(L"Open the Agent Manager") == std::wstring::npos && helpIn.find(L"1. Click \x2699 (Settings)") != std::wstring::npos &&
                  helpIn.find(L"3. Click Save.") != std::wstring::npos,
              "help: over the Agent Manager the open step is left out and the steps renumber");
    }

    // --- the shipped templates: ten, in order; the five OpenRouter ones tested, the Xiaomi pair, the NanoGPT pair and
    // the RunPod one not ---
    {
        const auto& all = LaunchPresetTemplates();
        const std::vector<std::wstring> order{ L"mimo-pro.openrouter", L"mimo-flash.openrouter", L"kimi.openrouter",    L"qwen.openrouter",
                                               L"glm.openrouter",      L"mimo-pro.xiaomi",       L"mimo-flash.xiaomi",  L"mimo-flash.nanogpt",
                                               L"glm-flash.nanogpt",   L"mimo-flash.runpod" };
        bool inOrder = all.size() == order.size();
        for (size_t i = 0; inOrder && i < all.size(); ++i)
        {
            inOrder = all[i].id == order[i] && (i < 5) == !all[i].tested.empty();
        }
        CHECK(inOrder, "templates: ten, in order; the five OpenRouter ones tested, the Xiaomi (R3), NanoGPT and RunPod ones not");
        const LaunchPresetsConfig empty{};
        CHECK(LaunchPresetTemplatesNotInList(empty, true) == std::vector<std::wstring>(order.begin(), order.begin() + 5), "templates: Add all tested = the five tested ones, in order");
        CHECK(LaunchPresetTemplatesNotInList(empty, false) == order, "templates: an empty list offers every template");
    }

    // --- one click adds a template: a LINKED preset at the end, visible, with a "p-" id and nothing typed; the list
    // then knows it (its row reads Added) and "Add all tested" skips it ---
    {
        LaunchPresetsConfig cfg;
        LaunchPreset mine;
        mine.id = L"p-0000aaaa";
        mine.fields[kPresetFieldName] = L"Mine";
        cfg.presets.push_back(mine);
        const auto added = AddLaunchPresetsFromTemplates(cfg, { L"kimi.openrouter" }, 42);
        CHECK(added.size() == 1 && cfg.presets.size() == 2 && cfg.presets[1].id == added[0], "add: one preset, appended after the user's own");
        const LaunchPreset kimi = cfg.presets[1]; // a copy: the adds below grow the vector
        CHECK(kimi.id.size() == 10 && kimi.id.rfind(L"p-", 0) == 0 && kimi.templateId == L"kimi.openrouter" && !kimi.hidden && kimi.fields.empty(),
              "add: a p- id, linked to the template, visible, nothing typed");
        CHECK(PresetDisplayName(kimi) == L"Kimi K3 \x00B7 OpenRouter", "add: its name follows the template");
        CHECK(LaunchPresetListHasTemplate(cfg, L"kimi.openrouter") && !LaunchPresetListHasTemplate(cfg, L"qwen.openrouter") && !LaunchPresetListHasTemplate(cfg, L""),
              "add: the list knows the template is in it");
        CHECK(!ShouldOfferAddPresets(cfg), "add: a list with presets offers no Add presets...");
        const auto rest = LaunchPresetTemplatesNotInList(cfg, true);
        CHECK(rest == std::vector<std::wstring>({ L"mimo-pro.openrouter", L"mimo-flash.openrouter", L"qwen.openrouter", L"glm.openrouter" }),
              "add: Add all tested now skips Kimi");
        const auto more = AddLaunchPresetsFromTemplates(cfg, rest, 42); // the same entropy as before: the ids must still differ
        bool unique = more.size() == 4 && cfg.presets.size() == 6;
        for (size_t i = 0; unique && i < cfg.presets.size(); ++i)
        {
            for (size_t j = i + 1; unique && j < cfg.presets.size(); ++j)
            {
                unique = cfg.presets[i].id != cfg.presets[j].id;
            }
        }
        CHECK(unique && cfg.presets[2].templateId == L"mimo-pro.openrouter" && cfg.presets[5].templateId == L"glm.openrouter",
              "add all: the four others appended in order, every id unique even from one entropy value");
        CHECK(LaunchPresetTemplatesNotInList(cfg, true).empty() &&
                  LaunchPresetTemplatesNotInList(cfg, false) == std::vector<std::wstring>({ L"mimo-pro.xiaomi", L"mimo-flash.xiaomi", L"mimo-flash.nanogpt", L"glm-flash.nanogpt", L"mimo-flash.runpod" }),
              "add all: no tested template is left; the five untested ones are still offered, by their own rows");
        const auto entries = LaunchMenuEntriesFrom({ { L"Opus", L"opus" } }, cfg);
        CHECK(entries.size() == 7 && entries[1].first == L"Mine" && entries[2].second == PresetToken(kimi.id) && entries[2].first == L"Kimi K3 \x00B7 OpenRouter",
              "add: the menus list the added presets in list order, under the template's name");
        CHECK(AddLaunchPresetsFromTemplates(cfg, { L"no-such.template" }, 7).empty() && cfg.presets.size() == 6, "add: an unknown template id adds nothing");
        CHECK(AddLaunchPresetsFromTemplates(cfg, { L"glm.openrouter" }, 7).size() == 1 && cfg.presets.size() == 7,
              "add: a template already in the list is added AGAIN when asked (Settings offers Add only when it is not)");
        const auto back = LaunchPresetsConfigFromJson(LaunchPresetsConfigToJson(cfg));
        CHECK(back.presets.size() == 7 && back.presets[1].templateId == L"kimi.openrouter" && back.presets[1].fields.empty(),
              "add: the added presets survive the JSON round trip, still linked and untyped");
    }

    // --- the one-time cleanup of a file written by release 0.6.8.18, the build that added the templates by itself:
    // a preset that is still EXACTLY what that build added is left out; anything the user touched stays ---
    {
        const std::wstring old =
            L"{\"version\":1,\"seededTemplates\":[\"mimo-pro.openrouter\",\"mimo-flash.openrouter\",\"mimo-pro.xiaomi\",\"mimo-flash.xiaomi\"],\"providers\":[],"
            L"\"presets\":["
            L"{\"id\":\"mimo-pro.openrouter\",\"templateId\":\"mimo-pro.openrouter\"}," // untouched -> left out
            L"{\"id\":\"mimo-flash.openrouter\",\"templateId\":\"mimo-flash.openrouter\",\"fields\":{\"name\":\"Flash\"}}," // renamed -> kept
            L"{\"id\":\"mimo-pro.xiaomi\",\"templateId\":\"mimo-pro.xiaomi\",\"hidden\":true}," // untouched (hidden, as added) -> left out
            L"{\"id\":\"mimo-flash.xiaomi\",\"templateId\":\"mimo-flash.xiaomi\"}," // shown by the user -> kept
            L"{\"id\":\"mimo-pro-2\",\"templateId\":\"mimo-pro.openrouter\"}," // a linked copy with another id -> kept
            L"{\"id\":\"p-0000aaaa\",\"fields\":{\"name\":\"Mine\"}}" // the user's own -> kept
            L"]}";
        std::vector<std::wstring> dropped;
        const auto cfg = LaunchPresetsConfigFromJson(old, &dropped);
        CHECK(dropped == std::vector<std::wstring>({ L"mimo-pro.openrouter", L"mimo-pro.xiaomi" }), "cleanup: exactly the untouched auto-added presets are left out");
        CHECK(cfg.presets.size() == 4 && cfg.presets[0].id == L"mimo-flash.openrouter" && cfg.presets[1].id == L"mimo-flash.xiaomi" &&
                  cfg.presets[2].id == L"mimo-pro-2" && cfg.presets[3].id == L"p-0000aaaa",
              "cleanup: a renamed one, an unhidden one, a linked copy and the user's own stay, in order");
        const auto text = LaunchPresetsConfigToJson(cfg);
        CHECK(text.find(L"seededTemplates") == std::wstring::npos, "cleanup: a Save writes no seededTemplates list");
        std::vector<std::wstring> again{ L"stale" };
        const auto reread = LaunchPresetsConfigFromJson(text, &again);
        CHECK(again.empty() && reread.presets.size() == 4, "cleanup: a file this build wrote is never pruned (and the out list is cleared)");
        // The same presets WITHOUT the list (a file no build filled by itself): nothing is left out.
        std::wstring noList = old;
        const auto from = noList.find(L"\"seededTemplates\"");
        noList.erase(from, noList.find(L"\"providers\"") - from);
        CHECK(LaunchPresetsConfigFromJson(noList).presets.size() == 6, "cleanup: runs only on a file that records seededTemplates");
        // The file release 0.6.8.18 wrote on the reference machine (2026-10-06 23:05, a Settings Save), verbatim: its
        // four presets are exactly what that build added, so the list reads EMPTY and the menus offer "Add presets...".
        const auto reference = LaunchPresetsConfigFromJson(
            L"{\"version\":1,\"seededTemplates\":[\"mimo-pro.openrouter\",\"mimo-flash.openrouter\",\"mimo-pro.xiaomi\",\"mimo-flash.xiaomi\"],\"providers\":[],"
            L"\"presets\":[{\"id\":\"mimo-pro.openrouter\",\"templateId\":\"mimo-pro.openrouter\"},{\"id\":\"mimo-flash.openrouter\",\"templateId\":\"mimo-flash.openrouter\"},"
            L"{\"id\":\"mimo-pro.xiaomi\",\"templateId\":\"mimo-pro.xiaomi\",\"hidden\":true},{\"id\":\"mimo-flash.xiaomi\",\"templateId\":\"mimo-flash.xiaomi\",\"hidden\":true}]}");
        CHECK(reference.presets.empty() && ShouldOfferAddPresets(reference), "cleanup: the reference machine's 0.6.8.18 file reads as an empty list");
    }

    // --- launch-presets.json round trip + tolerance ---
    {
        auto cfg = AllTemplatesConfig();
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
        CHECK(LaunchPresetsConfigToJson(cfg).find(L"seededTemplates") == std::wstring::npos && back.presets[0].id == L"mimo-pro.openrouter",
              "presets json: no seededTemplates list is written, so a re-read keeps every preset (nothing is pruned)");
        CHECK(back.providers.size() == 2 && back.providers[0].custom && back.providers[0].envPrefix == L"GLM", "presets json: a custom provider survives");
        CHECK(back.providers[1].anthropicBaseUrl == L"http://127.0.0.1:9999", "presets json: a built-in override survives");
        CHECK(LaunchPresetsConfigFromJson(L"not json").presets.empty(), "presets json: malformed text reads as an empty config");
        CHECK(LaunchPresetsConfigFromJson(L"").presets.empty(), "presets json: an empty file reads as an empty config");
        CHECK(LaunchPresetsConfigFromJson(L"{\"presets\":[{\"id\":\"a\"},{\"id\":\"a\"},{\"x\":1}]}").presets.size() == 1, "presets json: duplicate ids and id-less entries are dropped");
    }

    // --- tiers: box > Agentmaster env > Windows env > template ---
    {
        const auto cfg = AllTemplatesConfig();
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
        auto cfg = AllTemplatesConfig();
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
        const auto cfg = AllTemplatesConfig();
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
        // The launcher sets no effort; its tests ran with CLAUDE_CODE_EFFORT_LEVEL=max from the user's settings.json,
        // so the template carries that level itself, as the variable (which Claude Code obeys over --effort) + the flag.
        CHECK(EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == L"max" && c.effort == L"max", "compile: effort max rides the settings file and the --effort flag");
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

    // --- every OpenRouter template == its launcher profile (~/.claude/scripts/openrouter-profiles.json, 2026-10-06):
    // the request body (the profile's extraBody, verbatim below, compared as ORDERED JSON), every model tier, the
    // /model switch entry, the limits the launcher computed (OPENROUTER_DRY_RUN=1: context = the smallest window
    // among the providers in the order; max output = min(131072, provider max, context / 4)), effort max, no
    // identity note, nothing dropped ---
    {
        struct ProfileRef
        {
            const wchar_t* templateId;
            const wchar_t* model; // model == smallModel in every profile
            const wchar_t* switchModel; // nullptr = the profile has none
            const wchar_t* context;
            const wchar_t* extraBody; // the profile's extraBody as the launcher sends it
        };
        static constexpr ProfileRef kProfiles[] = {
            { L"mimo-pro.openrouter", L"xiaomi/mimo-v2.6-pro", L"xiaomi/mimo-v2.6-flash", L"1048576",
              L"{\"provider\":{\"order\":[\"gmicloud\",\"novita\",\"deepinfra\"],\"quantizations\":[\"bf16\",\"fp8\"]},\"temperature\":1.0,\"top_p\":0.95}" },
            { L"mimo-flash.openrouter", L"xiaomi/mimo-v2.6-flash", L"xiaomi/mimo-v2.6-pro", L"1048320",
              L"{\"provider\":{\"order\":[\"gmicloud\",\"novita\",\"deepinfra\",\"io-net\"],\"quantizations\":[\"bf16\",\"fp8\"]},\"temperature\":1.0,\"top_p\":0.95}" },
            { L"kimi.openrouter", L"moonshotai/kimi-k3", nullptr, L"1048576",
              L"{\"provider\":{\"order\":[\"decart\",\"modal\",\"deepinfra\"],\"quantizations\":[\"mxfp4\"]},\"temperature\":1.0,\"top_p\":1.0}" },
            { L"qwen.openrouter", L"qwen/qwen3.8-2.4t-a95b", nullptr, L"1000000",
              L"{\"provider\":{\"order\":[\"siliconflow\",\"novita\"],\"require_parameters\":true,\"allow_fallbacks\":false},\"temperature\":1.0,\"top_p\":0.95,\"top_k\":20}" },
            { L"glm.openrouter", L"z-ai/glm-5.3", nullptr, L"1048576",
              L"{\"provider\":{\"order\":[\"z-ai\",\"sail-research\",\"siliconflow\",\"akashml\"],\"quantizations\":[\"fp8\",\"bf16\"]},\"temperature\":1.0,\"top_p\":1.0}" },
        };
        const auto cfg = AllTemplatesConfig();
        for (const auto& ref : kProfiles)
        {
            const auto* t = FindLaunchPresetTemplate(ref.templateId);
            const auto* p = FindLaunchPreset(cfg, ref.templateId);
            CHECK(t && p && !t->tested.empty() && !p->hidden, "profiles: a tested, visible template exists for every launcher profile");
            if (!t || !p)
            {
                continue;
            }
            const auto a = SnapshotPresetLaunch(*p, cfg, MapEnv({}));
            const auto c = CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"sk-or-v1-0123456789abcdef");
            CHECK(c.error.empty() && c.warnings.empty() && c.sessionSettings, "profiles: the template compiles with nothing dropped");
            CHECK(JsonTextSame(EnvOf(c, L"CLAUDE_CODE_EXTRA_BODY"), ref.extraBody), "profiles: the request body == the profile's extraBody (ordered JSON)");
            bool tiers = c.model == ref.model;
            for (const auto* k : { L"ANTHROPIC_MODEL", L"ANTHROPIC_DEFAULT_FABLE_MODEL", L"ANTHROPIC_DEFAULT_OPUS_MODEL", L"ANTHROPIC_DEFAULT_SONNET_MODEL", L"ANTHROPIC_DEFAULT_HAIKU_MODEL", L"CLAUDE_CODE_SUBAGENT_MODEL" })
            {
                tiers &= EnvOf(c, k) == ref.model;
            }
            CHECK(tiers, "profiles: every tier (and --model) runs the profile's model");
            CHECK(EnvOf(c, L"ANTHROPIC_CUSTOM_MODEL_OPTION") == (ref.switchModel ? std::wstring{ ref.switchModel } : std::wstring{ L"<absent>" }), "profiles: the /model switch entry only where the profile has one");
            CHECK(EnvOf(c, L"CLAUDE_CODE_MAX_CONTEXT_TOKENS") == ref.context && EnvOf(c, L"CLAUDE_CODE_MAX_OUTPUT_TOKENS") == L"131072", "profiles: the launcher's computed limits");
            CHECK(EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == L"max" && c.effort == L"max", "profiles: effort max, the level every profile was tested with");
            CHECK(c.appendSystemPrompt.empty() && EnvOf(c, L"ANTHROPIC_BASE_URL") == L"https://openrouter.ai/api" && a.routing, "profiles: OpenRouter with routing, no identity note");
        }
        // The exact text for Qwen, the one body with every new field: Agentmaster writes 1.0 as 1, and the key
        // order is the profile's (provider: order, require_parameters, allow_fallbacks; then the sampling).
        const auto qa = SnapshotPresetLaunch(*FindLaunchPreset(cfg, L"qwen.openrouter"), cfg, MapEnv({}));
        CHECK(EnvOf(CompileClaudeLaunch(qa, EffectiveLaunchValues(qa, MapEnv({})), L"k-0123456789abcdef"), L"CLAUDE_CODE_EXTRA_BODY") ==
                  L"{\"provider\":{\"order\":[\"siliconflow\",\"novita\"],\"require_parameters\":true,\"allow_fallbacks\":false},\"temperature\":1,\"top_p\":0.95,\"top_k\":20}",
              "profiles: Qwen's body text, byte for byte");
        CHECK(!JsonTextSame(L"{\"a\":1,\"b\":2}", L"{\"b\":2,\"a\":1}") && JsonTextSame(L"{\"t\":1}", L"{\"t\":1.0}"), "profiles: the comparison pins key order and reads 1 == 1.0");
        CHECK(PresetDisplayName(*FindLaunchPreset(cfg, L"kimi.openrouter")) == L"Kimi K3 \x00B7 OpenRouter" &&
                  PresetDisplayName(*FindLaunchPreset(cfg, L"qwen.openrouter")) == L"Qwen3.8 2.4T \x00B7 OpenRouter" &&
                  PresetDisplayName(*FindLaunchPreset(cfg, L"glm.openrouter")) == L"GLM-5.3 \x00B7 OpenRouter",
              "profiles: the new menu names");
    }

    // --- effort: CLAUDE_CODE_EFFORT_LEVEL + --effort; "none" sets neither; an Extra env line still wins ---
    {
        const auto cfg = AllTemplatesConfig();
        auto p = *FindLaunchPreset(cfg, L"kimi.openrouter");
        const auto compile = [&](const LaunchPreset& preset) {
            const auto a = SnapshotPresetLaunch(preset, cfg, MapEnv({}));
            return CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"sk-or-v1-0123456789abcdef");
        };
        p.fields[kPresetFieldEffort] = L"None";
        auto c = compile(p);
        CHECK(c.effort.empty() && EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == L"<absent>" && c.warnings.empty(), "effort: \"none\" unsets the template's level (no flag, no variable, no warning)");
        p.fields[kPresetFieldEffort] = L"ultra";
        c = compile(p);
        CHECK(c.effort.empty() && EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == L"<absent>" && c.warnings.size() == 1, "effort: an unknown level is dropped with a warning");
        p.fields[kPresetFieldEffort] = L"High";
        c = compile(p);
        CHECK(c.effort == L"high" && EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == L"high" && c.warnings.empty(), "effort: a typed level replaces the template's, lower-cased, in both places");
        p.fields[kPresetFieldExtraEnv] = L"CLAUDE_CODE_EFFORT_LEVEL=low";
        c = compile(p);
        CHECK(c.effort == L"high" && EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == L"low", "effort: an Extra env line replaces the generated variable (the extra-env rule)");
        CHECK(c.warnings.size() == 1 && c.warnings[0].find(L"CLAUDE_CODE_EFFORT_LEVEL=low") != std::wstring::npos, "effort: ...and the preview says the Effort box no longer decides");
        p.fields[kPresetFieldExtraEnv] = L"claude_code_effort_level=HIGH";
        c = compile(p);
        CHECK(c.warnings.empty() && EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == L"HIGH", "effort: an Extra env line with the same level (any case) is no conflict");
    }

    // --- top_k + require_parameters: validation, scope, and the extra-body merge ---
    {
        const auto cfg = AllTemplatesConfig();
        const auto compile = [&](const LaunchPreset& preset, std::wstring_view key) {
            const auto a = SnapshotPresetLaunch(preset, cfg, MapEnv({}));
            return CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), key);
        };
        auto q = *FindLaunchPreset(cfg, L"qwen.openrouter");
        q.fields[kPresetFieldTopK] = L"20.5";
        q.fields[kPresetFieldRequireParameters] = L"yes";
        auto c = compile(q, L"k-0123456789abcdef");
        CHECK(EnvOf(c, L"CLAUDE_CODE_EXTRA_BODY") == L"{\"provider\":{\"order\":[\"siliconflow\",\"novita\"],\"allow_fallbacks\":false},\"temperature\":1,\"top_p\":0.95}",
              "body: a fractional top_k and a non-boolean require_parameters are dropped, not guessed");
        CHECK(c.warnings.size() == 2, "body: ...each with a warning");
        q = *FindLaunchPreset(cfg, L"qwen.openrouter");
        q.fields[kPresetFieldExtraBody] = L"{\"top_k\":40,\"provider\":{\"require_parameters\":false}}";
        c = compile(q, L"k-0123456789abcdef");
        CHECK(EnvOf(c, L"CLAUDE_CODE_EXTRA_BODY") == L"{\"provider\":{\"order\":[\"siliconflow\",\"novita\"],\"require_parameters\":false,\"allow_fallbacks\":false},\"temperature\":1,\"top_p\":0.95,\"top_k\":40}",
              "body: the extra body deep-merges over the new fields, in place");
        // Xiaomi has no routing block: require_parameters is out of scope there (never snapshotted, never sent), top_k is not.
        auto x = *FindLaunchPreset(cfg, L"mimo-pro.xiaomi");
        x.fields[kPresetFieldTopK] = L"40";
        x.fields[kPresetFieldRequireParameters] = L"true";
        const auto xa = SnapshotPresetLaunch(x, cfg, MapEnv({}));
        CHECK(xa.set.find(kPresetFieldRequireParameters) == xa.set.end() && xa.set.at(kPresetFieldTopK) == L"40", "scope: require_parameters is OpenRouter-only, top_k any provider");
        CHECK(EnvOf(CompileClaudeLaunch(xa, EffectiveLaunchValues(xa, MapEnv({})), L"tp-xxxxxxxxxxxxxxxx"), L"CLAUDE_CODE_EXTRA_BODY") == L"{\"temperature\":1,\"top_p\":0.95,\"top_k\":40}",
              "scope: a Xiaomi body carries top_k and no provider block");
        // The Claude login takes no request-body fields at all.
        LaunchPreset login;
        login.id = L"p-login2";
        login.fields[kPresetFieldTopK] = L"20";
        CHECK(SnapshotPresetLaunch(login, cfg, MapEnv({})).set.empty(), "scope: top_k is provider-only (never on the Claude login)");
    }

    // --- Xiaomi: no routing block; Claude login: flags only ---
    {
        const auto cfg = AllTemplatesConfig();
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
        CHECK(lc.error.empty() && lc.model == L"opus" && lc.effort == L"max", "login: model + effort ride the command line");
        CHECK(lc.sessionSettings && lc.settingsEnv.size() == 1 && EnvOf(lc, L"CLAUDE_CODE_EFFORT_LEVEL") == L"max",
              "login: the effort ALSO rides a settings file, as its only env value (a user's CLAUDE_CODE_EFFORT_LEVEL would otherwise override --effort)");
        CHECK(lc.authTokenEnv.empty() && la.set.find(kPresetFieldTemperature) == la.set.end(), "login: no token, provider-only fields skipped");
        login.fields.erase(kPresetFieldEffort);
        const auto la2 = SnapshotPresetLaunch(login, cfg, MapEnv({}));
        CHECK(!CompileClaudeLaunch(la2, EffectiveLaunchValues(la2, MapEnv({})), L"").sessionSettings, "login: a model-only preset needs no settings file (the plain --model launch)");
        login.fields[kPresetFieldMaxOutput] = L"64000";
        const auto la3 = SnapshotPresetLaunch(login, cfg, MapEnv({}));
        CHECK(CompileClaudeLaunch(la3, EffectiveLaunchValues(la3, MapEnv({})), L"").sessionSettings, "login: an env value needs the settings file");
    }

    // --- the built-in provider table: four providers, in Settings order, each found by id ---
    {
        const std::vector<std::wstring> ids{ L"openrouter", L"xiaomi", L"nanogpt", L"runpod" };
        bool table = std::size(kBuiltInLaunchProviders) == ids.size();
        for (size_t i = 0; table && i < ids.size(); ++i)
        {
            table = kBuiltInLaunchProviders[i].id == ids[i] && FindBuiltInLaunchProvider(ids[i]) == &kBuiltInLaunchProviders[i] &&
                    kBuiltInLaunchProviders[i].name && *kBuiltInLaunchProviders[i].name && SanitizeLaunchEnvPrefix(kBuiltInLaunchProviders[i].envPrefix) == kBuiltInLaunchProviders[i].envPrefix;
        }
        CHECK(table, "providers: OpenRouter, Xiaomi MiMo, NanoGPT, RunPod - in order, found by id, each with a name and a clean env prefix");
        CHECK(!FindBuiltInLaunchProvider(L"") && !FindBuiltInLaunchProvider(kAnthropicProviderId) && !FindBuiltInLaunchProvider(L"c-12345678") && !FindBuiltInLaunchProvider(L"NanoGPT"),
              "providers: the Claude login, a custom id and a display name are no built-in id (ids are exact)");
    }

    // --- NanoGPT: the direct API host, NANOGPT_API_KEY, no routing block; both templates are the NORMAL models (not the
    // refusal-removed copies the user's evaluation ran) with the values of NanoGPT's catalog ---
    {
        const auto cfg = AllTemplatesConfig();
        const auto prov = ResolveLaunchProvider(cfg, L"nanogpt");
        CHECK(prov.found && !prov.routing && !prov.custom && !prov.modelRequired && prov.name == L"NanoGPT" && prov.envPrefix == L"NANOGPT" && prov.baseUrlExample.empty(),
              "nanogpt: a built-in provider without routing, prefix NANOGPT, any model allowed");
        const auto url = ResolveProviderBaseUrl(prov, nullptr, nullptr, true);
        CHECK(url.value == L"https://api.nano-gpt.com/api" && url.source == PresetValueSource::ProviderDefault && url.envName == L"NANOGPT_BASE_URL",
              "nanogpt: the default Base URL is the direct API host's Anthropic root (claude appends /v1/messages)");
        struct NanoRef
        {
            const wchar_t* templateId;
            const wchar_t* model;
            const wchar_t* switchModel;
            const wchar_t* effort;
            const wchar_t* body;
        };
        static constexpr NanoRef kNano[] = {
            { L"mimo-flash.nanogpt", L"xiaomi/mimo-v2.6-flash", L"xiaomi/mimo-v2.6-pro", L"high", L"{\"temperature\":1,\"top_p\":0.95}" },
            { L"glm-flash.nanogpt", L"z-ai/glm-5.3-flash", L"z-ai/glm-5.3", L"max", L"{\"temperature\":1,\"top_p\":1}" },
        };
        for (const auto& ref : kNano)
        {
            const auto* p = FindLaunchPreset(cfg, ref.templateId);
            CHECK(p != nullptr, "nanogpt: the template exists");
            if (!p)
            {
                continue;
            }
            CHECK(PresetLaunchRefusal(*p, cfg).empty(), "nanogpt: the template launches");
            const auto a = SnapshotPresetLaunch(*p, cfg, MapEnv({}));
            CHECK(a.providerId == L"nanogpt" && !a.routing && a.keyCredential == L"nanogpt" && a.keyEnv == L"NANOGPT_API_KEY", "nanogpt: the key is NANOGPT_API_KEY (the variable NanoGPT's own examples use)");
            const auto c = CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"nano-0123456789abcdef");
            CHECK(c.error.empty() && c.warnings.empty() && c.sessionSettings, "nanogpt: the template compiles with nothing dropped");
            CHECK(EnvOf(c, L"ANTHROPIC_BASE_URL") == L"https://api.nano-gpt.com/api", "nanogpt: the session goes to the direct API host");
            bool tiers = c.model == ref.model;
            for (const auto* k : { L"ANTHROPIC_MODEL", L"ANTHROPIC_DEFAULT_FABLE_MODEL", L"ANTHROPIC_DEFAULT_OPUS_MODEL", L"ANTHROPIC_DEFAULT_SONNET_MODEL", L"ANTHROPIC_DEFAULT_HAIKU_MODEL", L"CLAUDE_CODE_SUBAGENT_MODEL" })
            {
                tiers &= EnvOf(c, k) == ref.model;
            }
            const std::wstring model{ ref.model };
            CHECK(tiers && model.find(L"abliterated") == std::wstring::npos && model.find(L"uncensored") == std::wstring::npos,
                  "nanogpt: every tier runs the NORMAL model (no abliterated / uncensored id)");
            CHECK(EnvOf(c, L"ANTHROPIC_CUSTOM_MODEL_OPTION") == ref.switchModel, "nanogpt: /model switches to the full model of the family");
            CHECK(EnvOf(c, L"CLAUDE_CODE_MAX_CONTEXT_TOKENS") == L"1000000" && EnvOf(c, L"CLAUDE_CODE_MAX_OUTPUT_TOKENS") == L"131072",
                  "nanogpt: context 1,000,000 (the smallest host window but Engy's) and the catalog's 131,072 output");
            CHECK(EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == ref.effort && c.effort == ref.effort, "nanogpt: the top effort level the model lists (MiMo: high; GLM: max)");
            CHECK(EnvOf(c, L"CLAUDE_CODE_EXTRA_BODY") == ref.body, "nanogpt: the body carries the vendor's sampling and no provider block");
        }
        CHECK(PresetModelDisplayName(L"xiaomi/mimo-v2.6-flash:reasoning-effort/none", L"NanoGPT") == L"mimo-v2.6-flash:reasoning-effort/none (NanoGPT)",
              "display name: a slash inside a NanoGPT suffix is not the vendor prefix");
        CHECK(PresetModelDisplayName(L"deepseek/deepseek-r1:free", L"OpenRouter") == L"deepseek-r1:free (OpenRouter)" &&
                  PresetModelDisplayName(L"us.anthropic.claude-x:0", L"") == L"us.anthropic.claude-x:0",
              "display name: ids with a ':' and no slash after it read as before");
    }

    // --- RunPod: the user's own vLLM endpoint - no default Base URL (a refusal that shows the URL shapes and the
    // variable), RUNPOD_API_KEY, exactly one model (a preset without one is refused when picked) ---
    {
        const auto cfg = AllTemplatesConfig();
        const auto prov = ResolveLaunchProvider(cfg, L"runpod");
        CHECK(prov.found && !prov.routing && prov.modelRequired && prov.baseUrlDefault.empty() && prov.envPrefix == L"RUNPOD" &&
                  prov.baseUrlExample.find(L".api.runpod.ai") != std::wstring::npos && prov.baseUrlExample.find(L".proxy.runpod.net") != std::wstring::npos,
              "runpod: no default URL, an example of both endpoint shapes, one model per endpoint");
        const auto none = ResolveProviderBaseUrl(prov, nullptr, nullptr, false);
        CHECK(none.value.empty() && none.source == PresetValueSource::None && none.envName == L"RUNPOD_BASE_URL", "runpod: nothing resolves until the user names the endpoint");

        const auto* p = FindLaunchPreset(cfg, L"mimo-flash.runpod");
        CHECK(p != nullptr, "runpod: the template exists");
        if (p)
        {
            CHECK(PresetLaunchRefusal(*p, cfg).empty(), "runpod: the template names its model, so it is not refused when picked");
            const auto a = SnapshotPresetLaunch(*p, cfg, MapEnv({}));
            CHECK(a.keyEnv == L"RUNPOD_API_KEY" && a.env.at(kPresetFieldBaseUrl) == L"RUNPOD_BASE_URL" && a.base.find(kPresetFieldBaseUrl) == a.base.end(),
                  "runpod: the key is RUNPOD_API_KEY; the Base URL is only the variable's NAME (no default to fall back to)");
            const auto refused = CompileClaudeLaunch(a, EffectiveLaunchValues(a, MapEnv({})), L"rpa_0123456789abcdef");
            CHECK(refused.error.find(L"No Base URL for RunPod") == 0 && refused.error.find(L"https://<ENDPOINT_ID>.api.runpod.ai") != std::wstring::npos &&
                      refused.error.find(L"or set RUNPOD_BASE_URL.") != std::wstring::npos,
                  "runpod: no URL = refused, with the endpoint shapes and the variable in the message");

            // RUNPOD_BASE_URL set in Windows: re-read at this launch (tier 3), and the session runs on the endpoint.
            const auto windows = MapEnv({ { L"RUNPOD_BASE_URL", L"https://abc123xyz.api.runpod.ai" } });
            const auto c = CompileClaudeLaunch(a, EffectiveLaunchValues(a, windows), L"rpa_0123456789abcdef");
            CHECK(c.error.empty() && c.warnings.empty() && EnvOf(c, L"ANTHROPIC_BASE_URL") == L"https://abc123xyz.api.runpod.ai", "runpod: the Windows variable names the endpoint");
            bool tiers = c.model == L"XiaomiMiMo/MiMo-V2.6-Flash-MOPD";
            for (const auto* k : { L"ANTHROPIC_MODEL", L"ANTHROPIC_DEFAULT_FABLE_MODEL", L"ANTHROPIC_DEFAULT_OPUS_MODEL", L"ANTHROPIC_DEFAULT_SONNET_MODEL", L"ANTHROPIC_DEFAULT_HAIKU_MODEL", L"CLAUDE_CODE_SUBAGENT_MODEL" })
            {
                tiers &= EnvOf(c, k) == L"XiaomiMiMo/MiMo-V2.6-Flash-MOPD";
            }
            CHECK(tiers, "runpod: every tier asks for the one Hugging Face model the server runs");
            CHECK(EnvOf(c, L"ANTHROPIC_CUSTOM_MODEL_OPTION") == L"<absent>" && EnvOf(c, L"CLAUDE_CODE_EFFORT_LEVEL") == L"<absent>" && c.effort.empty(),
                  "runpod: no /model switch entry (the server has no other model) and no effort");
            CHECK(EnvOf(c, L"CLAUDE_CODE_MAX_CONTEXT_TOKENS") == L"917504" && EnvOf(c, L"CLAUDE_CODE_MAX_OUTPUT_TOKENS") == L"131072",
                  "runpod: the benchmark-like limits - 128K output (Claude Code's cap) and the 1M window minus that output");
            // vLLM refuses a request whose input plus max_tokens passes --max-model-len (1,048,576 in the documented
            // server command), so the window Claude Code plans compaction around must leave room for a full answer.
            const auto ctx = std::wcstoull(EnvOf(c, L"CLAUDE_CODE_MAX_CONTEXT_TOKENS").c_str(), nullptr, 10);
            const auto out = std::wcstoull(EnvOf(c, L"CLAUDE_CODE_MAX_OUTPUT_TOKENS").c_str(), nullptr, 10);
            CHECK(ctx > 0 && out > 0 && ctx + out <= 1048576ull, "runpod: context + max output fit the server's --max-model-len 1048576");
            CHECK(EnvOf(c, L"CLAUDE_CODE_EXTRA_BODY") == L"{\"temperature\":1,\"top_p\":0.95}" && EnvOf(c, L"ANTHROPIC_DEFAULT_OPUS_MODEL_NAME") == L"MiMo-V2.6-Flash-MOPD (RunPod)",
                  "runpod: Xiaomi's sampling; the /model display name drops the Hugging Face owner");

            // The URL typed into the provider's box is frozen into the session (tier 1).
            auto typed = cfg;
            LaunchProviderEntry entry;
            entry.id = L"runpod";
            entry.anthropicBaseUrl = L"  https://pod77-8000.proxy.runpod.net  ";
            typed.providers.push_back(entry);
            const auto ta = SnapshotPresetLaunch(*FindLaunchPreset(typed, L"mimo-flash.runpod"), typed, MapEnv({}));
            CHECK(ta.set.at(kPresetFieldBaseUrl) == L"https://pod77-8000.proxy.runpod.net" &&
                      EnvOf(CompileClaudeLaunch(ta, EffectiveLaunchValues(ta, windows), L"vllm-key-0123456789"), L"ANTHROPIC_BASE_URL") == L"https://pod77-8000.proxy.runpod.net",
                  "runpod: a typed Pod URL is frozen into the session and beats the Windows variable");
        }

        // A RunPod preset that names no model is refused when picked; the template's model, or a typed one, launches.
        LaunchPreset blank;
        blank.id = L"p-0000cafe";
        blank.fields[kPresetFieldName] = L"My pod";
        blank.fields[kPresetFieldProvider] = L"runpod";
        const auto refusal = PresetLaunchRefusal(blank, cfg);
        CHECK(refusal.find(L"\"My pod\"") != std::wstring::npos && refusal.find(L"RunPod") != std::wstring::npos && refusal.find(L"Main model") != std::wstring::npos,
              "runpod: a preset with no model is refused, naming the preset, the provider and the box to fill");
        blank.fields[kPresetFieldModel] = L"   ";
        CHECK(!PresetLaunchRefusal(blank, cfg).empty(), "runpod: a model box of blanks is no model");
        blank.fields[kPresetFieldModel] = L"Qwen/Qwen3-Coder-30B-A3B-Instruct";
        CHECK(PresetLaunchRefusal(blank, cfg).empty(), "runpod: a typed Hugging Face id launches");
        LaunchPreset linked;
        linked.id = L"p-0000d00d";
        linked.templateId = L"mimo-flash.runpod";
        CHECK(PresetLaunchRefusal(linked, cfg).empty(), "runpod: a preset linked to the template takes the template's model");
        LaunchPreset other;
        other.id = L"p-0000f00d";
        other.fields[kPresetFieldProvider] = L"nanogpt";
        CHECK(PresetLaunchRefusal(other, cfg).empty(), "runpod rule: a provider that serves many models launches without one (Claude Code's own ids)");
    }

    // --- validation: extra env, extra args, numbers ---
    {
        const auto cfg = AllTemplatesConfig();
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
        auto cfg = AllTemplatesConfig();
        const auto entries = LaunchMenuEntriesFrom({ { L"Opus", L"opus" } }, cfg);
        CHECK(entries.size() == 6 && entries[0].second == L"opus", "menus: launch models first");
        CHECK(entries[1].second == L"@mimo-pro.openrouter" && entries[2].second == L"@mimo-flash.openrouter" && entries[3].second == L"@kimi.openrouter" &&
                  entries[4].second == L"@qwen.openrouter" && entries[5].second == L"@glm.openrouter",
              "menus: then the VISIBLE presets as @ tokens (the hidden Xiaomi pair is not listed)");
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
        CHECK(IsValidPresetEffort(L"xHigh") && !IsValidPresetEffort(L"ultra") && !IsValidPresetEffort(L"none"), "effort levels (\"none\" is the compile step's off value, not a level)");
    }

    // --- the /handover per-message hint (ParseHandoverArgsHints, COMMANDS.md §6b) with the presets in its spec: a
    // preset is picked ONLY through a bracketed PREFIX of its name, never by a bare word. Every template name ends
    // in "OpenRouter", and under the substring rule "open the PR" or "out of memory" switched the successor to a
    // paid provider ---
    {
        const auto cfg = AllTemplatesConfig();
        const auto spec = LaunchMenuSpecFrom(L"Fable | fable\nOpus | opus\nSonnet | sonnet", cfg);
        const auto model = [&](std::wstring_view args) { return ParseHandoverArgsHints(args, spec).modelId; };
        CHECK(model(L"[mimo] fix x") == L"@mimo-pro.openrouter" && model(L"[MiMo Flash] fix x") == L"@mimo-flash.openrouter", "hint: [mimo] picks the first MiMo preset, [mimo flash] the Flash one");
        CHECK(model(L"[kimi] x") == L"@kimi.openrouter" && model(L"[qwen] x") == L"@qwen.openrouter" && model(L"[glm 5.3] x") == L"@glm.openrouter", "hint: the new templates by a prefix of their names");
        const auto titled = ParseHandoverArgsHints(L"[glm] [review pass] x", spec);
        CHECK(titled.modelId == L"@glm.openrouter" && titled.title == L"review pass", "hint: the title slot still follows a preset pick");
        bool noAccident = true;
        for (const auto* args : { L"open the PR and merge it", L"out of memory in the worker", L"pro tips first", L"router config is broken", L"outer loop rewrite", L"mimo fix the bug", L"kimi integration fails", L"glm results look off" })
        {
            noAccident &= model(args).empty();
        }
        CHECK(noAccident, "hint: a bare word never picks a preset (the provider - and the bill - would change by accident)");
        const auto router = ParseHandoverArgsHints(L"[router] x", spec);
        CHECK(router.modelId.empty() && router.title == L"router", "hint: a bracket that only occurs INSIDE a preset name is a title, not a pick");
        CHECK(model(L"[openrouter] x").empty() && model(L"[flash] x").empty(), "hint: the provider name or a mid-name word picks nothing");
        CHECK(model(L"opus please refactor") == L"opus" && model(L"[son] x") == L"sonnet", "hint: the launch models keep the substring and bare rules");
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
        const auto cfg = AllTemplatesConfig();
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
