// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — PRESETS: named, user-editable configurations of a Claude session
// (doc/agentmaster/HARNESSES.md).
//
// A preset says HOW one Claude session runs: which provider serves it (OpenRouter, NanoGPT, Xiaomi MiMo, a RunPod
// endpoint that serves a Hugging Face model through vLLM, a custom Anthropic-compatible endpoint, or the user's own
// Claude login), which model each tier uses, its effort, the
// limits, the request-body fields (OpenRouter routing, sampling, extra JSON), an optional identity note for a
// non-Claude model, extra env vars and extra claude arguments. Every "Open New Session Here" / "Fork session"
// menu and the Launch button's hover menu list the visible presets after a separator. A pick travels through
// the existing model-pick plumbing as the string "@<presetId>" (IsPresetToken), so no menu callback changes
// shape.
//
// Three layers of data:
//   * TEMPLATES (LaunchPresetTemplates) — tested configurations shipped with the app. NONE is in the user's
//     list by default (the user's rule, 2026-10-06: "the initial presets are none"): Settings ▸ Presets ▸
//     TEMPLATES adds one, or every tested one, with one click (AddLaunchPresetsFromTemplates), and while the
//     list is empty every launch menu offers "Add presets…" where the presets would be (ShouldOfferAddPresets),
//     which tells where that list is. An added preset stays LINKED to its template: an empty field inherits
//     the template's CURRENT value, so a release that retunes a template reaches every preset whose field
//     the user never typed.
//   * PRESETS (LaunchPresetsConfig, <profile>\launch-presets.json) — the user's list: add, rename, hide,
//     delete. Fields are SPARSE: only what the user typed is stored; the rest resolves through the tiers.
//   * LAUNCH ARGS (LaunchArgs, <profile>\session-store\<sid>.json key "launchArgs") — the FLATTENED
//     snapshot a session takes at its first launch and reuses on every later launch (resume, restart,
//     fork, /handover, /clear). Editing or deleting a preset, or a template update, never changes a
//     session that exists.
//
// Value tiers (the user's rule, 2026-10-06). For each field the first tier that is set wins:
//   1. the field's box in the preset                      -> frozen into LaunchArgs::set
//   2. Agentmaster's env list (Settings: Global + Per-dir) -> frozen into LaunchArgs::set
//   3. the Windows environment                            -> only the variable NAME is stored
//                                                           (LaunchArgs::env); re-read at EVERY launch
//   4. the template, or the provider's default            -> frozen into LaunchArgs::base (applies when the
//                                                           tier-3 variable is absent at that launch)
// The API KEY never sits in a snapshot: LaunchArgs keeps WHERE to look (the Credential Manager target,
// then the variable name in Agentmaster's env, then in Windows), and every launch reads it again —
// otherwise the key would be plain text in every session file.
//
// What a provider preset compiles to (CompileClaudeLaunch): Claude Code copies the env block of its
// settings files over its process environment, and the user's ~/.claude/settings.json may pin model ids
// (it pins `model`, ANTHROPIC_DEFAULT_OPUS_MODEL and CLAUDE_CODE_MAX_OUTPUT_TOKENS on the reference
// machine), so the model, base URL and limits go into the `--settings` file's env block: --settings
// outranks the user, project and local settings files. Claude reads ONE --settings value and Agentmaster
// already passes its hooks file there, so a preset's file carries the hooks too (ClaudeSpawn.cpp,
// MaterializeLaunchSettingsFile). The key rides the process environment only, as ANTHROPIC_AUTH_TOKEN
// (a Bearer token): a non-Anthropic key in ANTHROPIC_API_KEY opens Claude Code's "Detected a custom API key
// in your environment" modal (claude.exe 2.1.291), which an unattended tab cannot answer.
//
// PURE + header-only (the ModelCatalog.h idiom): no file, registry or credential access here — the OS half
// is LaunchPresetsOs.h — so the standalone test harness covers all of it (tests/tests_presets.cpp).
// Exceptions: like the STL it is built on, any function here can throw std::bad_alloc on memory
// exhaustion; nothing else throws (every parse failure degrades to a documented empty/default result).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Json.h"

namespace Agentmaster
{
    // ===== field keys ========================================================================
    // One key per preset field. The SAME key names the box in the Settings tab, the sparse entry in
    // launch-presets.json, a template's value, and the entry in a LaunchArgs snapshot, so one field
    // table (LaunchPresetFieldSpecs) drives the UI, the resolver and the compile step. Renaming a key
    // orphans every stored preset and snapshot that used it — only ever add keys.

    inline constexpr const wchar_t* kPresetFieldName = L"name"; // the menu label
    inline constexpr const wchar_t* kPresetFieldProvider = L"provider"; // a provider id, kAnthropicProviderId, or "" (inherit)
    inline constexpr const wchar_t* kPresetFieldModel = L"model"; // the main model (and every tier that is not overridden)
    inline constexpr const wchar_t* kPresetFieldSmallModel = L"smallModel"; // the haiku tier / background work
    inline constexpr const wchar_t* kPresetFieldSwitchModel = L"switchModel"; // an extra /model picker entry; "none" = no entry
    inline constexpr const wchar_t* kPresetFieldEffort = L"effort"; // CLAUDE_CODE_EFFORT_LEVEL + --effort <level>; "none" = neither
    inline constexpr const wchar_t* kPresetFieldContext = L"context"; // CLAUDE_CODE_MAX_CONTEXT_TOKENS
    inline constexpr const wchar_t* kPresetFieldMaxOutput = L"maxOutput"; // CLAUDE_CODE_MAX_OUTPUT_TOKENS
    inline constexpr const wchar_t* kPresetFieldProviderOrder = L"providerOrder"; // OpenRouter provider.order
    inline constexpr const wchar_t* kPresetFieldQuantizations = L"quantizations"; // OpenRouter provider.quantizations
    inline constexpr const wchar_t* kPresetFieldRequireParameters = L"requireParameters"; // OpenRouter provider.require_parameters
    inline constexpr const wchar_t* kPresetFieldAllowFallbacks = L"allowFallbacks"; // OpenRouter provider.allow_fallbacks
    inline constexpr const wchar_t* kPresetFieldTemperature = L"temperature"; // request body temperature
    inline constexpr const wchar_t* kPresetFieldTopP = L"topP"; // request body top_p
    inline constexpr const wchar_t* kPresetFieldTopK = L"topK"; // request body top_k (a whole number)
    inline constexpr const wchar_t* kPresetFieldExtraBody = L"extraBody"; // a JSON object merged into the request body
    inline constexpr const wchar_t* kPresetFieldExtraEnv = L"extraEnv"; // NAME=VALUE lines for the settings env block
    inline constexpr const wchar_t* kPresetFieldIdentityNote = L"identityNote"; // --append-system-prompt text; "none" = no note
    inline constexpr const wchar_t* kPresetFieldExtraArgs = L"extraArgs"; // raw claude arguments
    // The provider's Base URL. Not a preset field (it lives on the provider), but the snapshot carries it
    // under this key so a session keeps the URL it started with.
    inline constexpr const wchar_t* kPresetFieldBaseUrl = L"baseUrl";

    // The explicit "use the user's own Claude login" provider. A preset's provider box holds it when the
    // user picks that choice over a template's provider; an empty box means "inherit the template's".
    inline constexpr const wchar_t* kAnthropicProviderId = L"anthropic";

    // The process variables a preset launch adds to the ConPTY environment. AM_PRESET names the preset
    // (the observer stamp, never secret). AM_PRESET_TOKEN carries the key for ONE hop only: the pwsh
    // host script moves it into ANTHROPIC_AUTH_TOKEN for the claude call and removes both afterwards
    // (BuildPwshHostedCommandline), so the pwsh prompt left in the tab never holds a key.
    inline constexpr const wchar_t* kPresetIdEnvName = L"AM_PRESET";
    inline constexpr const wchar_t* kPresetTokenEnvName = L"AM_PRESET_TOKEN";

    // The session-store key (SessionStore.h) that holds a session's LaunchArgs as a JSON string.
    inline constexpr const wchar_t* kSessionStoreLaunchArgsKey = L"launchArgs";

    // A preset pick's prefix in the model-pick plumbing: "@<presetId>". No Anthropic, Bedrock, Vertex,
    // OpenRouter, NanoGPT, Xiaomi or Hugging Face model id starts with '@', so a token can never be mistaken
    // for a model.
    inline constexpr wchar_t kPresetTokenPrefix = L'@';

    // No template ships an identity note (the user, 2026-10-06: "not necessary"). The reference launcher
    // (~/.claude/scripts/openrouter.ps1) passes one so a non-Claude model does not call itself Claude; here the
    // Identity note box is optional and empty by default, so a preset launch appends no system prompt unless
    // the user typed one (RenderPresetIdentityNote fills its placeholders).

    // ===== small pure helpers ===============================================================

    namespace detail
    {
        // True for the whitespace the preset boxes may carry around a value (a TextBox reports a typed
        // newline as \r). Never throws.
        inline bool PresetIsSpace(wchar_t c) noexcept
        {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
        }

        // `s` without leading/trailing PresetIsSpace characters.
        //   s - any text.
        // Returns the trimmed copy ("" when `s` is all whitespace).
        inline std::wstring PresetTrim(std::wstring_view s)
        {
            size_t a = 0;
            size_t b = s.size();
            while (a < b && PresetIsSpace(s[a]))
            {
                ++a;
            }
            while (b > a && PresetIsSpace(s[b - 1]))
            {
                --b;
            }
            return std::wstring{ s.substr(a, b - a) };
        }

        // ASCII-only lower-casing: env names, flags and provider ids are ASCII, and a locale-aware fold
        // would make the comparison depend on the user's culture. Never throws.
        inline wchar_t PresetLowerAscii(wchar_t c) noexcept
        {
            return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        }

        // ASCII case-insensitive equality — Windows env names compare case-insensitively.
        //   a, b - the two strings.
        // Returns true when they are equal ignoring ASCII case. Never throws.
        inline bool PresetEqualsCi(std::wstring_view a, std::wstring_view b) noexcept
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (PresetLowerAscii(a[i]) != PresetLowerAscii(b[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // True when `s` is a non-empty run of ASCII digits — the only token limits accept (a sign, a
        // decimal point or a thousands separator would become a silently wrong Claude Code limit).
        //   s - the candidate.
        // Never throws.
        inline bool PresetIsAllDigits(std::wstring_view s) noexcept
        {
            if (s.empty() || s.size() > 12)
            {
                return false; // 12 digits already exceeds every real context window; longer is a typo
            }
            for (const wchar_t c : s)
            {
                if (c < L'0' || c > L'9')
                {
                    return false;
                }
            }
            return true;
        }

        // Parse a decimal number the way a user types one ("1", "1.0", "0.95", ".5"). Rejects anything
        // with trailing text, so "0.95x" or "1,0" never becomes a silently different sampling value.
        //   s - the text (already trimmed by the caller).
        // Returns the value, or nullopt for empty / malformed / non-finite input. Never throws.
        inline std::optional<double> PresetParseDecimal(std::wstring_view s)
        {
            if (s.empty() || s.size() > 32)
            {
                return std::nullopt;
            }
            wchar_t buf[40]{};
            std::copy(s.begin(), s.end(), buf);
            wchar_t* end = nullptr;
            const double v = std::wcstod(buf, &end);
            if (end == buf || end == nullptr || *end != L'\0' || !(v == v) || v > 1e300 || v < -1e300)
            {
                return std::nullopt; // nothing parsed, trailing junk, NaN, or an overflow
            }
            return v;
        }

        // Split a list the user typed ("gmicloud, novita deepinfra") on commas, semicolons and
        // whitespace; trimmed, empties dropped, order kept.
        //   s - the text.
        // Returns the items.
        inline std::vector<std::wstring> PresetSplitList(std::wstring_view s)
        {
            std::vector<std::wstring> out;
            std::wstring cur;
            for (const wchar_t c : s)
            {
                if (c == L',' || c == L';' || PresetIsSpace(c))
                {
                    if (!cur.empty())
                    {
                        out.push_back(std::move(cur));
                        cur.clear();
                    }
                    continue;
                }
                cur.push_back(c);
            }
            if (!cur.empty())
            {
                out.push_back(std::move(cur));
            }
            return out;
        }

        // True for a name a settings env block may carry: [A-Za-z_][A-Za-z0-9_]* (the env editor's rule).
        //   name - the candidate.
        // Never throws.
        inline bool PresetIsValidEnvName(std::wstring_view name) noexcept
        {
            if (name.empty())
            {
                return false;
            }
            for (size_t i = 0; i < name.size(); ++i)
            {
                const wchar_t c = name[i];
                const bool alpha = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_';
                const bool digit = c >= L'0' && c <= L'9';
                if (!(alpha || (i > 0 && digit)))
                {
                    return false;
                }
            }
            return true;
        }

        // True for a name a preset's extra env may NOT set. CCMGR_* is the hook correlation (claude copies a
        // settings env block into its own process env, so a preset value would reach the hook forwarder and
        // mis-file every hook); AM_* and WT_SESSION are Agentmaster's ownership stamps.
        //   name - the env name.
        // Never throws.
        inline bool PresetIsReservedEnvName(std::wstring_view name) noexcept
        {
            const auto startsCi = [&](std::wstring_view prefix) {
                return name.size() >= prefix.size() && PresetEqualsCi(name.substr(0, prefix.size()), prefix);
            };
            return startsCi(L"CCMGR_") || startsCi(L"AM_") || PresetEqualsCi(name, L"WT_SESSION");
        }

        // Parse the extra-env box: NAME=VALUE entries separated by newlines or ';', '#' comments and blank
        // entries skipped, NAME trimmed, VALUE trimmed (the env editor's ParseEnvAssignments rule, repeated
        // here so this header stays independent of ClaudeSpawn).
        //   text - the box text.
        //   bad  - receives one note per entry that was skipped as malformed or reserved (may be null).
        // Returns the accepted pairs, in order (a later duplicate is kept; the compile step lets it win).
        inline std::vector<std::pair<std::wstring, std::wstring>> PresetParseEnvLines(std::wstring_view text, std::vector<std::wstring>* bad)
        {
            std::vector<std::pair<std::wstring, std::wstring>> out;
            size_t i = 0;
            while (i <= text.size())
            {
                size_t sep = i;
                while (sep < text.size() && text[sep] != L'\n' && text[sep] != L'\r' && text[sep] != L';')
                {
                    ++sep;
                }
                const std::wstring entry = PresetTrim(text.substr(i, sep - i));
                if (!entry.empty() && entry.front() != L'#')
                {
                    const size_t eq = entry.find(L'=');
                    const std::wstring name = PresetTrim(eq == std::wstring::npos ? std::wstring_view{} : std::wstring_view{ entry }.substr(0, eq));
                    if (eq == std::wstring::npos || !PresetIsValidEnvName(name))
                    {
                        if (bad)
                        {
                            bad->push_back(L"extra env: skipped \"" + entry + L"\" (not NAME=VALUE)");
                        }
                    }
                    else if (PresetIsReservedEnvName(name))
                    {
                        if (bad)
                        {
                            bad->push_back(L"extra env: skipped " + name + L" (reserved for Agentmaster)");
                        }
                    }
                    else
                    {
                        out.emplace_back(name, PresetTrim(std::wstring_view{ entry }.substr(eq + 1)));
                    }
                }
                if (sep >= text.size())
                {
                    break;
                }
                i = sep + 1;
            }
            return out;
        }

        // FNV-1a over the UTF-16 code units of `s` — the cheap content hash behind the launch settings
        // file names (ClaudeSpawn.cpp) and fresh preset ids. Not cryptographic and not meant to be.
        //   s - the text.
        //   h - the running hash (chain calls by passing the previous result).
        // Returns the 64-bit hash. Never throws.
        inline uint64_t PresetFnv1a(std::wstring_view s, uint64_t h = 1469598103934665603ull) noexcept
        {
            for (const wchar_t c : s)
            {
                h ^= static_cast<uint64_t>(c);
                h *= 1099511628211ull;
            }
            return h;
        }

        // `value` as `digits` lowercase hex characters (the low bits).
        //   value  - the number.
        //   digits - how many hex digits to render (1..16).
        // Returns the hex text.
        inline std::wstring PresetHex(uint64_t value, int digits)
        {
            static constexpr wchar_t kHex[] = L"0123456789abcdef";
            std::wstring out(static_cast<size_t>(digits), L'0');
            for (int i = digits - 1; i >= 0; --i)
            {
                out[static_cast<size_t>(i)] = kHex[value & 0xF];
                value >>= 4;
            }
            return out;
        }

        // A number rendered for JSON with no precision loss and no noise: the shortest of %.15g/%.17g that
        // reads back to the same double. Json.h's printer uses %g (6 significant digits), which would turn
        // a typed 0.123456789 into 0.123457 inside CLAUDE_CODE_EXTRA_BODY.
        //   v - the number.
        // Returns the text ("0" for a non-finite value, which JSON cannot express).
        inline std::wstring PresetJsonNumber(double v)
        {
            if (!(v == v) || v > 1e308 || v < -1e308)
            {
                return L"0";
            }
            if (v == static_cast<double>(static_cast<int64_t>(v)) && v < 9e15 && v > -9e15)
            {
                return std::to_wstring(static_cast<int64_t>(v));
            }
            wchar_t buf[64]{};
            std::swprintf(buf, 64, L"%.15g", v);
            if (std::wcstod(buf, nullptr) != v)
            {
                std::swprintf(buf, 64, L"%.17g", v);
            }
            return buf;
        }

        // Json.h's compact printer, but with PresetJsonNumber for numbers (see there).
        //   v   - the value.
        //   out - receives the text (appended).
        inline void PresetDumpJson(const json::Value& v, std::wstring& out)
        {
            switch (v.type)
            {
            case json::Value::Type::Null:
                out += L"null";
                break;
            case json::Value::Type::Bool:
                out += v.boolean ? L"true" : L"false";
                break;
            case json::Value::Type::Num:
                out += PresetJsonNumber(v.num);
                break;
            case json::Value::Type::Str:
                json::detail::Escape(v.str, out);
                break;
            case json::Value::Type::Arr:
                out += L'[';
                for (size_t k = 0; k < v.arr.size(); ++k)
                {
                    if (k)
                    {
                        out += L',';
                    }
                    PresetDumpJson(v.arr[k], out);
                }
                out += L']';
                break;
            case json::Value::Type::Obj:
                out += L'{';
                for (size_t k = 0; k < v.members.size(); ++k)
                {
                    if (k)
                    {
                        out += L',';
                    }
                    json::detail::Escape(v.members[k].first, out);
                    out += L':';
                    PresetDumpJson(v.members[k].second, out);
                }
                out += L'}';
                break;
            }
        }

        // Set `key` in object `obj`, REPLACING an existing member (json::Value::Set only appends, which would
        // emit a duplicate key that a JSON reader resolves to whichever copy it saw last).
        //   obj   - an object value (anything else is left alone).
        //   key   - the member name.
        //   value - the new value.
        inline void PresetJsonPut(json::Value& obj, const std::wstring& key, json::Value value)
        {
            if (obj.type != json::Value::Type::Obj)
            {
                return;
            }
            for (auto& [k, v] : obj.members)
            {
                if (k == key)
                {
                    v = std::move(value);
                    return;
                }
            }
            obj.members.emplace_back(key, std::move(value));
        }
    }

    // Deep-merge `over` into `base` like jq's `*`: two objects merge member by member, recursively; any
    // other pair (arrays included) is REPLACED by `over`'s value. This is the reference launcher's rule for
    // OPENROUTER_EXTRA_BODY over a profile's body, kept so a body that works there behaves the same here.
    //   base - the value to merge into (modified).
    //   over - the value whose members win.
    inline void PresetJsonDeepMerge(json::Value& base, const json::Value& over)
    {
        if (base.type != json::Value::Type::Obj || over.type != json::Value::Type::Obj)
        {
            base = over;
            return;
        }
        for (const auto& [k, v] : over.members)
        {
            auto it = std::find_if(base.members.begin(), base.members.end(), [&](const auto& m) { return m.first == k; });
            if (it == base.members.end())
            {
                base.members.emplace_back(k, v);
            }
            else if (it->second.type == json::Value::Type::Obj && v.type == json::Value::Type::Obj)
            {
                PresetJsonDeepMerge(it->second, v);
            }
            else
            {
                it->second = v;
            }
        }
    }

    // Mask a secret for display: enough of its start to recognize the kind of key ("sk-or-v1-…") and its
    // last 4 characters to tell two keys apart, never the middle. Used by every placeholder, the launch
    // preview and the hover panel, so a screenshot or a shared screen never shows a usable key.
    //   key - the secret.
    // Returns "" for an empty key; "•••" for a key too short to show any part safely.
    inline std::wstring MaskSecret(std::wstring_view key)
    {
        if (key.empty())
        {
            return {};
        }
        if (key.size() < 12)
        {
            return L"\x2022\x2022\x2022"; // •••
        }
        // The visible head ends at the last '-' within the first 9 characters ("sk-or-v1-", "tp-"), so the
        // prefix that names the key KIND shows while no secret characters after it do.
        size_t head = 0;
        for (size_t i = 0; i < 9 && i < key.size(); ++i)
        {
            if (key[i] == L'-')
            {
                head = i + 1;
            }
        }
        return std::wstring{ key.substr(0, head) } + L"\x2026" + std::wstring{ key.substr(key.size() - 4) };
    }

    // The display name of a model in Claude Code's /model picker: the id without a vendor prefix, then the
    // provider in parentheses ("xiaomi/mimo-v2.6-pro" through OpenRouter -> "mimo-v2.6-pro (OpenRouter)").
    // Only a '/' BEFORE the first ':' ends the vendor prefix: NanoGPT's model suffixes can hold a slash of their
    // own ("xiaomi/mimo-v2.6-flash:reasoning-effort/none" turns MiMo's reasoning off), and the last slash would
    // shorten that id to "none".
    //   id           - the model id.
    //   providerName - the provider's display name ("" = no suffix).
    // Returns the display text ("" for an empty id).
    inline std::wstring PresetModelDisplayName(std::wstring_view id, std::wstring_view providerName)
    {
        if (id.empty())
        {
            return {};
        }
        // npos when there is no ':' (substr then keeps the whole id); a slash after the first ':' is the suffix's.
        const size_t slash = id.substr(0, id.find(L':')).find_last_of(L'/');
        std::wstring out{ slash == std::wstring_view::npos ? id : id.substr(slash + 1) };
        if (!providerName.empty())
        {
            out += L" (" + std::wstring{ providerName } + L")";
        }
        return out;
    }

    // Fill the placeholders of an identity note the user typed into a preset.
    //   text         - the note with {model} / {provider} / {switchModel} / {switch} placeholders.
    //   model        - the main model id.
    //   providerName - the provider's display name.
    //   switchModel  - the /model switch entry ("" = none; {switch} then renders as nothing).
    // Returns the note, trimmed.
    inline std::wstring RenderPresetIdentityNote(std::wstring_view text, std::wstring_view model, std::wstring_view providerName, std::wstring_view switchModel)
    {
        std::wstring out{ text };
        const auto replaceAll = [&](std::wstring_view from, std::wstring_view to) {
            size_t pos = 0;
            while ((pos = out.find(from, pos)) != std::wstring::npos)
            {
                out.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        // {switch} first: its own expansion contains {switchModel}, which the later pass then fills.
        replaceAll(L"{switch}", switchModel.empty() ? std::wstring{} : std::wstring{ L", unless the user switched to {switchModel} with /model" });
        replaceAll(L"{switchModel}", switchModel);
        replaceAll(L"{model}", model);
        replaceAll(L"{provider}", providerName);
        return detail::PresetTrim(out);
    }

    // True for an effort level claude 2.1.291 accepts both as --effort (`claude --help`: low, medium, high, xhigh,
    // max) and as CLAUDE_CODE_EFFORT_LEVEL (its level list Kc). The preset value "none" is not a level: the compile
    // step reads it as "set no effort" before it asks this.
    //   level - the value (case-insensitive).
    // Never throws.
    inline bool IsValidPresetEffort(std::wstring_view level) noexcept
    {
        for (const auto* ok : { L"low", L"medium", L"high", L"xhigh", L"max" })
        {
            if (detail::PresetEqualsCi(level, ok))
            {
                return true;
            }
        }
        return false;
    }

    // The first claude flag in `args` that a preset may not pass, because Agentmaster owns it: the session
    // id and resume/fork recipe (a second value would split the session from its record), the settings
    // file (it carries the hooks), the model (the preset's own fields set it), and print mode (it would
    // turn the interactive tab into a one-shot run).
    //   args - the extra-arguments box.
    // Returns the offending flag as typed, or "" when the arguments are acceptable.
    inline std::wstring PresetForbiddenArgument(std::wstring_view args)
    {
        static constexpr const wchar_t* kForbidden[] = { L"--settings", L"--session-id", L"--resume", L"-r", L"--fork-session", L"--continue", L"-c", L"--model", L"-p", L"--print" };
        size_t i = 0;
        while (i < args.size())
        {
            while (i < args.size() && detail::PresetIsSpace(args[i]))
            {
                ++i;
            }
            size_t j = i;
            while (j < args.size() && !detail::PresetIsSpace(args[j]))
            {
                ++j;
            }
            std::wstring_view token = args.substr(i, j - i);
            const size_t eq = token.find(L'=');
            const std::wstring_view flag = eq == std::wstring_view::npos ? token : token.substr(0, eq);
            for (const auto* bad : kForbidden)
            {
                if (detail::PresetEqualsCi(flag, bad))
                {
                    return std::wstring{ token };
                }
            }
            i = j;
        }
        return {};
    }

    // ===== providers =========================================================================

    // A provider Agentmaster knows without any setup. The defaults are the values the reference launcher
    // and the providers' documentation use (doc/agentmaster/HARNESSES.md §9); the user can override the Base URL.
    struct LaunchProviderDef
    {
        const wchar_t* id; // stable id: what presets and snapshots store
        const wchar_t* name; // display name ("OpenRouter")
        const wchar_t* envPrefix; // <PREFIX>_API_KEY, <PREFIX>_BASE_URL, <PREFIX>_SMALL_MODEL, ...
        const wchar_t* anthropicBaseUrl; // the Anthropic-compatible endpoint (claude appends /v1/messages); "" = the user's own
        const wchar_t* openaiBaseUrl; // the OpenAI-compatible endpoint — for Codex presets later; unused today
        const wchar_t* keyHint; // what a key looks like, for the key box's placeholder
        bool routing; // true = OpenRouter's `provider` routing block exists (order / quantizations / fallbacks)
        // The shape of the user's own endpoint, for a provider with no default URL ("" = none): the Base URL box's
        // placeholder while nothing resolves, and the "No Base URL" refusal's example.
        const wchar_t* baseUrlExample;
        // True = the endpoint serves exactly ONE model, the one its server runs (vLLM's served model name), so a
        // preset without a model is refused when it is picked (PresetLaunchRefusal): Claude Code would otherwise
        // ask for its own Claude model ids, which such a server answers with "model does not exist".
        bool modelRequired;
    };

    // The built-in providers, in the order Settings lists them.
    //   * Xiaomi's Token Plan uses another URL (https://token-plan-cn.xiaomimimo.com/anthropic, or the region URL on
    //     the plan page) and tp-/ttp- keys: the user types that URL into the provider's Base URL box.
    //   * NanoGPT: the direct API host, which NanoGPT recommends for long requests (a higher request size limit and
    //     a longer request runtime than nano-gpt.com; its API Hosts page, read 2026-10-09). Claude Code appends
    //     /v1/messages: https://api.nano-gpt.com/api/v1/messages answered 401 without a key on 2026-10-09, an
    //     unknown path 404. NanoGPT's own examples name the key variable NANOGPT_API_KEY, which the prefix gives.
    //     No routing block: NanoGPT documents its `provider` object for /v1/chat/completions, not /v1/messages.
    //     A host is picked by appending ":<provider id>" to the model id instead (billed pay-as-you-go, +5%).
    //   * RunPod: the user's own endpoint, so there is no default URL. A load-balancing Serverless endpoint
    //     (https://<ENDPOINT_ID>.api.runpod.ai) takes the RunPod API key as its Bearer token and passes the
    //     request to the worker's server; a Pod's HTTP proxy (https://<POD_ID>-<port>.proxy.runpod.net) reaches
    //     the server directly, and vLLM then checks the --api-key it was started with. The server must speak
    //     Claude's /v1/messages (vLLM and SGLang do) and serves exactly one model, hence modelRequired. A
    //     queue-based Serverless endpoint speaks only OpenAI's API and cannot serve Claude Code.
    inline constexpr LaunchProviderDef kBuiltInLaunchProviders[] = {
        { L"openrouter", L"OpenRouter", L"OPENROUTER", L"https://openrouter.ai/api", L"https://openrouter.ai/api/v1", L"sk-or-v1-\x2026", true, L"", false },
        { L"xiaomi", L"Xiaomi MiMo", L"MIMO", L"https://api.xiaomimimo.com/anthropic", L"https://api.xiaomimimo.com/v1", L"sk-\x2026 (pay-as-you-go) or tp-\x2026 (Token Plan)", false, L"", false },
        { L"nanogpt", L"NanoGPT", L"NANOGPT", L"https://api.nano-gpt.com/api", L"https://api.nano-gpt.com/api/v1", L"the key from nano-gpt.com/api", false, L"", false },
        { L"runpod", L"RunPod", L"RUNPOD", L"", L"", L"rpa_\x2026 for a Serverless endpoint; for a Pod, its vLLM --api-key", false,
          L"https://<ENDPOINT_ID>.api.runpod.ai, or https://<POD_ID>-8000.proxy.runpod.net", true },
    };

    // The built-in provider with `id`.
    //   id - a provider id ("" never matches).
    // Returns a pointer into kBuiltInLaunchProviders, or nullptr for a custom or unknown id. Never throws.
    inline const LaunchProviderDef* FindBuiltInLaunchProvider(std::wstring_view id) noexcept
    {
        if (id.empty())
        {
            return nullptr;
        }
        for (const auto& d : kBuiltInLaunchProviders)
        {
            if (id == d.id)
            {
                return &d;
            }
        }
        return nullptr;
    }

    // The user's stored data for one provider: an override of a built-in one (only the URLs are used), or
    // a custom provider (`custom` = true; every field is the user's).
    struct LaunchProviderEntry
    {
        std::wstring id; // a built-in id, or "c-<8 hex>" for a custom provider
        bool custom{ false };
        std::wstring name; // custom only (a built-in keeps its own name)
        std::wstring envPrefix; // custom only; sanitized to [A-Z0-9_]
        std::wstring anthropicBaseUrl; // the Base URL box ("" = inherit <PREFIX>_BASE_URL, then the default)
        std::wstring openaiBaseUrl; // stored for Codex presets later; unused today
    };

    // A provider resolved for use: the built-in definition merged with the user's entry.
    struct ResolvedLaunchProvider
    {
        bool found{ false }; // false = unknown id (a deleted custom provider); every other field is then empty
        bool custom{ false };
        bool routing{ false };
        std::wstring id;
        std::wstring name;
        std::wstring envPrefix;
        std::wstring keyHint;
        std::wstring baseUrlBox; // the user's Base URL box (may be empty)
        std::wstring baseUrlDefault; // the built-in default ("" for a custom provider, and for RunPod)
        std::wstring baseUrlExample; // the built-in's LaunchProviderDef::baseUrlExample ("" for a custom provider)
        bool modelRequired{ false }; // the built-in's LaunchProviderDef::modelRequired (false for a custom provider)
    };

    // ===== presets, templates, config ========================================================

    // One preset in the user's list. `fields` is SPARSE: a key is present only when the user typed a value.
    struct LaunchPreset
    {
        std::wstring id; // stable: what a menu pick ("@<id>") and a snapshot name; a rename never changes it
        std::wstring templateId; // "" = a blank preset; else the template whose values fill empty fields
        bool hidden{ false }; // true = not listed in the launch menus (still editable in Settings)
        std::map<std::wstring, std::wstring> fields; // field key -> the value the user typed
    };

    // A shipped, tested configuration. It is never in the user's list by itself: Settings ▸ Presets ▸ TEMPLATES
    // adds it on a click (AddLaunchPresetsFromTemplates), as a preset LINKED to it.
    struct LaunchPresetTemplate
    {
        std::wstring id; // stable: the link a preset keeps (LaunchPreset::templateId); never reused for other values
        std::wstring tested; // when + with which Claude Code it was verified; "" = not verified yet ("Add all tested" skips it)
        std::map<std::wstring, std::wstring> fields; // every value the template sets
    };

    // The whole <profile>\launch-presets.json document. An empty `presets` is the INITIAL state: no preset until
    // the user adds one (a template or a blank preset in Settings).
    struct LaunchPresetsConfig
    {
        std::vector<LaunchProviderEntry> providers; // overrides of built-ins + custom providers
        std::vector<LaunchPreset> presets; // menu order
    };

    // The templates this release ships, in the order Settings lists them. The five OpenRouter templates are the reference
    // launcher's profiles (~/.claude/scripts/openrouter-profiles.json: mimo, mimo-flash, kimi, qwen, glm), each
    // run end to end through that launcher on 2026-10-06 with Claude Code 2.1.282: the endpoints in each order
    // returned thinking tokens and received the earlier reasoning, and every request carried the template's
    // sampling values. MiMo Flash was re-checked on 2.1.291 the same day. HARNESSES.md §4 has the measurements
    // behind each choice (speed, price, precision).
    //
    // Effort is "max" in all five: the tests ran with CLAUDE_CODE_EFFORT_LEVEL=max in the user's own
    // ~/.claude/settings.json (the launcher sets no effort; OpenRouter maps "max" to each model's top reasoning
    // level, which is xhigh for Qwen). A template carries it so a machine without that variable runs the tested
    // configuration too. MiMo ignores the level (200 requests changed nothing), and setting it keeps the template
    // equal to what was tested.
    //
    // The Xiaomi pair uses the documented endpoint and ids but has NOT been run yet (HARNESSES.md check R3:
    // does Xiaomi accept a Bearer token?), so its `tested` is empty: Settings marks it "not tested yet", "Add all
    // tested" skips it, and it sets no effort.
    //
    // The NanoGPT pair (the user's request, 2026-10-09: NanoGPT "templated to normal models (not abliterated)") is
    // the NORMAL release of the two models the user's evaluation ran as refusal-removed copies on NanoGPT
    // (xiaomi/mimo-v2.6-flash-abliterated, z-ai/glm-5.3-flash-uncensored). Every value comes from NanoGPT's public
    // catalog and provider lists (GET /api/v1/models?detailed=true and /api/models/<id>/providers, read
    // 2026-10-09; HARNESSES.md §4.2): the ids; the effort levels each model lists (MiMo: none and high, so high;
    // GLM: low, high and max, so max); Max output 131,072 (the catalog's); Context 1,000,000, the smallest window
    // of MiMo's hosts (Venice) and of GLM's hosts but one (Runware and Nube; Engy's 262,144 is smaller, and Engy
    // states no precision, while NanoGPT labels its automatic route "FP8+"). Sampling and effort are the vendors'
    // benchmark settings (the model cards, read 2026-10-09): Xiaomi recommends temperature 1.0, top_p 0.95 for
    // MiMo, whose top NanoGPT effort level is high; Z.ai runs GLM-5.3 Flash's Claude Code benchmark (Terminal-Bench
    // 2.1, Claude Code 2.1.207) at temperature 1.0, top_p 1 and 65,536 output tokens, and max is the model's
    // default effort. No host is pinned: NanoGPT then routes automatically, tries to keep a session on one host
    // that caches prompts, and a NanoGPT subscription covers both models, while a pinned host is billed
    // pay-as-you-go plus 5%. Precision: Xiaomi's own MiMo V2.6 release already holds its experts in MXFP4, so no
    // host serves MiMo above Xiaomi's precision; GLM-5.3 Flash is released in FP8 and BF16, and 11 of its 36
    // NanoGPT hosts serve FP4 / NVFP4, but NanoGPT documents its precision filter (`provider.min_quantization`)
    // and its host pin (`X-Provider`) only for chat completions, not for the Messages API Claude Code uses
    // (HARNESSES.md §4.2, check R8). Not run through Claude Code yet, so `tested` is empty.
    //
    // DeepSeek V4.1 Flash on NanoGPT (the user's request, 2026-10-10: "best strongest parameters"), from DeepSeek's
    // model card (deepseek-ai/DeepSeek-V4.1-Flash, read 2026-10-10) and NanoGPT's catalog and host list (same day):
    //   * main model `deepseek/deepseek-v4.1-flash:thinking`, NanoGPT's id for the thinking mode, so the reasoning
    //     is on whatever the request carries; the background model is the plain id (titles and summaries need no
    //     reasoning). The /model switch entry is DeepSeek V4 Pro in its thinking mode.
    //   * Effort max: the card's results all use the maximum reasoning effort (its scale 1-100 at 100), and max is
    //     the top of the levels NanoGPT lists (none, low, high, max).
    //   * temperature 1.0, top_p 0.95: the card's evaluation sampling (its table allows top_p 0.95 or 1.0). The card
    //     runs its agent benchmarks in Claude Code too (DeepSWE v1.1 69.8, Terminal-Bench 2.1 88.0).
    //   * Context 1,000,000: the catalog's window and the smallest of the hosts that state a precision (Venice,
    //     Alibaba); only Kitani and Engy (262,144) are smaller, and neither states one. The card benchmarks a 1M
    //     window.
    //   * Max output 131,072: the card asks for max_tokens of at least 256K, but Claude Code asks a model it does
    //     not know for at most 128,000 tokens (the catalog allows 384,000).
    //   * Precision: DeepSeek's own release is FP8 with its experts in FP4 (510 GB; no higher-precision release
    //     exists), and NanoGPT labels its automatic route "FP8+", so no host is pinned; a NanoGPT subscription
    //     covers the model.
    // Not run through Claude Code yet, so `tested` is empty.
    //
    // The RunPod template serves a Hugging Face model the user deploys: Xiaomi's official MiMo V2.6 Flash, the
    // MOPD checkpoint (XiaomiMiMo/MiMo-V2.6-Flash-MOPD, published 2026-09-27; Xiaomi's note: a fix for repeated
    // tool calls, which matters most in Claude Code). The model id must be vLLM's served model name, the
    // repository id by default. The values are the benchmark-like ones (HARNESSES.md §4.2, §9):
    //   * the server runs Xiaomi's own vLLM command with `--max-model-len 1048576`, the model's full window. The
    //     weights take 172.9 GB (MXFP4 experts + FP8 attention: Xiaomi's release precision; no higher-precision
    //     release exists), and Xiaomi's command uses tensor parallel 4 (4 x H200 fits the 1M window either way).
    //   * Max output 131,072: Claude Code asks for at most 128,000 tokens for a model it does not know, the
    //     128K output Z.ai's own Claude Code benchmarks use.
    //   * Context 917,504 = 1,048,576 - 131,072. vLLM REFUSES a request whose input plus max_tokens passes its
    //     --max-model-len, so the window Claude Code plans compaction around leaves room for a full answer.
    //   * Sampling: Xiaomi's recommended temperature 1.0, top_p 0.95. No effort: MiMo's chat template thinks
    //     unless a request passes enable_thinking=false, and what vLLM does with Claude Code's effort level is not
    //     checked.
    // A load-balancing Serverless endpoint ends any request after 5.5 minutes: there, Max output 32,000 and
    // Context = the server's window - 32,000 (HARNESSES.md §9). Not run yet, so `tested` is empty.
    //
    // The list order is the order of Settings ▸ Presets ▸ TEMPLATES and of "Add all tested" (the launcher's order).
    // Returns the immutable list (built once, thread-safe).
    inline const std::vector<LaunchPresetTemplate>& LaunchPresetTemplates()
    {
        static const std::vector<LaunchPresetTemplate> templates = [] {
            std::vector<LaunchPresetTemplate> t;
            const std::wstring tested = L"2026-10-06 \x00B7 Claude Code 2.1.282";
            t.push_back({ L"mimo-pro.openrouter", tested, {
                { kPresetFieldName, L"MiMo Pro \x00B7 OpenRouter" },
                { kPresetFieldProvider, L"openrouter" },
                { kPresetFieldModel, L"xiaomi/mimo-v2.6-pro" },
                { kPresetFieldSmallModel, L"xiaomi/mimo-v2.6-pro" },
                { kPresetFieldSwitchModel, L"xiaomi/mimo-v2.6-flash" },
                { kPresetFieldEffort, L"max" },
                { kPresetFieldContext, L"1048576" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldProviderOrder, L"gmicloud, novita, deepinfra" },
                { kPresetFieldQuantizations, L"bf16, fp8" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"0.95" },
            } });
            t.push_back({ L"mimo-flash.openrouter", L"2026-10-06 \x00B7 Claude Code 2.1.282 and 2.1.291", {
                { kPresetFieldName, L"MiMo Flash \x00B7 OpenRouter" },
                { kPresetFieldProvider, L"openrouter" },
                { kPresetFieldModel, L"xiaomi/mimo-v2.6-flash" },
                { kPresetFieldSmallModel, L"xiaomi/mimo-v2.6-flash" },
                { kPresetFieldSwitchModel, L"xiaomi/mimo-v2.6-pro" },
                { kPresetFieldEffort, L"max" },
                { kPresetFieldContext, L"1048320" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldProviderOrder, L"gmicloud, novita, deepinfra, io-net" },
                { kPresetFieldQuantizations, L"bf16, fp8" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"0.95" },
            } });
            // Kimi K3: Moonshot's benchmark settings. Moonshot trains Kimi K3 in MXFP4, so "mxfp4 only" is the
            // official precision, not a quantized copy (the generic fp4 hosts stay out). Decart first: ~200 tokens/s.
            t.push_back({ L"kimi.openrouter", tested, {
                { kPresetFieldName, L"Kimi K3 \x00B7 OpenRouter" },
                { kPresetFieldProvider, L"openrouter" },
                { kPresetFieldModel, L"moonshotai/kimi-k3" },
                { kPresetFieldSmallModel, L"moonshotai/kimi-k3" },
                { kPresetFieldEffort, L"max" },
                { kPresetFieldContext, L"1048576" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldProviderOrder, L"decart, modal, deepinfra" },
                { kPresetFieldQuantizations, L"mxfp4" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"1.0" },
            } });
            // Qwen3.8-2.4T-A95B (the open-weight Qwen3.8 Max): Qwen's recommended sampling. require_parameters
            // skips a host that would ignore top_k (Modal); allow_fallbacks false keeps the request on the two
            // listed hosts — without it a forced failure went to Venice, whose precision is not stated. No
            // quantizations filter: Novita states no precision, so a filter would drop it.
            t.push_back({ L"qwen.openrouter", tested, {
                { kPresetFieldName, L"Qwen3.8 2.4T \x00B7 OpenRouter" },
                { kPresetFieldProvider, L"openrouter" },
                { kPresetFieldModel, L"qwen/qwen3.8-2.4t-a95b" },
                { kPresetFieldSmallModel, L"qwen/qwen3.8-2.4t-a95b" },
                { kPresetFieldEffort, L"max" },
                { kPresetFieldContext, L"1000000" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldProviderOrder, L"siliconflow, novita" },
                { kPresetFieldRequireParameters, L"true" },
                { kPresetFieldAllowFallbacks, L"false" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"0.95" },
                { kPresetFieldTopK, L"20" },
            } });
            // GLM-5.3: Z.ai's own Claude Code benchmark settings. Z.ai releases FP8 weights, and its own endpoint
            // (z-ai) is first; fallbacks stay on fp8/bf16.
            t.push_back({ L"glm.openrouter", tested, {
                { kPresetFieldName, L"GLM-5.3 \x00B7 OpenRouter" },
                { kPresetFieldProvider, L"openrouter" },
                { kPresetFieldModel, L"z-ai/glm-5.3" },
                { kPresetFieldSmallModel, L"z-ai/glm-5.3" },
                { kPresetFieldEffort, L"max" },
                { kPresetFieldContext, L"1048576" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldProviderOrder, L"z-ai, sail-research, siliconflow, akashml" },
                { kPresetFieldQuantizations, L"fp8, bf16" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"1.0" },
            } });
            t.push_back({ L"mimo-pro.xiaomi", L"", {
                { kPresetFieldName, L"MiMo Pro \x00B7 Xiaomi" },
                { kPresetFieldProvider, L"xiaomi" },
                { kPresetFieldModel, L"mimo-v2.6-pro" },
                { kPresetFieldSmallModel, L"mimo-v2.6-pro" },
                { kPresetFieldSwitchModel, L"mimo-v2.6-flash" },
                { kPresetFieldContext, L"1048576" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"0.95" },
            } });
            t.push_back({ L"mimo-flash.xiaomi", L"", {
                { kPresetFieldName, L"MiMo Flash \x00B7 Xiaomi" },
                { kPresetFieldProvider, L"xiaomi" },
                { kPresetFieldModel, L"mimo-v2.6-flash" },
                { kPresetFieldSmallModel, L"mimo-v2.6-flash" },
                { kPresetFieldSwitchModel, L"mimo-v2.6-pro" },
                { kPresetFieldContext, L"1048576" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"0.95" },
            } });
            // NanoGPT, the normal releases (see the comment above). The /model switch entry moves a session to the
            // full model of the same family, as the OpenRouter MiMo Flash template does.
            t.push_back({ L"mimo-flash.nanogpt", L"", {
                { kPresetFieldName, L"MiMo Flash \x00B7 NanoGPT" },
                { kPresetFieldProvider, L"nanogpt" },
                { kPresetFieldModel, L"xiaomi/mimo-v2.6-flash" },
                { kPresetFieldSmallModel, L"xiaomi/mimo-v2.6-flash" },
                { kPresetFieldSwitchModel, L"xiaomi/mimo-v2.6-pro" },
                { kPresetFieldEffort, L"high" },
                { kPresetFieldContext, L"1000000" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"0.95" },
            } });
            t.push_back({ L"glm-flash.nanogpt", L"", {
                { kPresetFieldName, L"GLM-5.3 Flash \x00B7 NanoGPT" },
                { kPresetFieldProvider, L"nanogpt" },
                { kPresetFieldModel, L"z-ai/glm-5.3-flash" },
                { kPresetFieldSmallModel, L"z-ai/glm-5.3-flash" },
                { kPresetFieldSwitchModel, L"z-ai/glm-5.3" },
                { kPresetFieldEffort, L"max" },
                { kPresetFieldContext, L"1000000" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"1.0" },
            } });
            // DeepSeek V4.1 Flash: the card's benchmark settings at the maximum effort (see the comment above). The
            // background model is the plain id on purpose: the thinking id would reason over every title.
            t.push_back({ L"deepseek-flash.nanogpt", L"", {
                { kPresetFieldName, L"DeepSeek V4.1 Flash \x00B7 NanoGPT" },
                { kPresetFieldProvider, L"nanogpt" },
                { kPresetFieldModel, L"deepseek/deepseek-v4.1-flash:thinking" },
                { kPresetFieldSmallModel, L"deepseek/deepseek-v4.1-flash" },
                { kPresetFieldSwitchModel, L"deepseek/deepseek-v4-pro:thinking" },
                { kPresetFieldEffort, L"max" },
                { kPresetFieldContext, L"1000000" },
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"0.95" },
            } });
            // RunPod: one Hugging Face model on the user's own vLLM endpoint (see the comment above). No /model
            // switch entry: the endpoint serves no other model.
            t.push_back({ L"mimo-flash.runpod", L"", {
                { kPresetFieldName, L"MiMo Flash \x00B7 RunPod" },
                { kPresetFieldProvider, L"runpod" },
                { kPresetFieldModel, L"XiaomiMiMo/MiMo-V2.6-Flash-MOPD" },
                { kPresetFieldSmallModel, L"XiaomiMiMo/MiMo-V2.6-Flash-MOPD" },
                { kPresetFieldContext, L"917504" }, // the server's 1,048,576 window minus Max output (vLLM refuses a request that passes it)
                { kPresetFieldMaxOutput, L"131072" },
                { kPresetFieldTemperature, L"1.0" },
                { kPresetFieldTopP, L"0.95" },
            } });
            return t;
        }();
        return templates;
    }

    // The template with `id`.
    //   id - a template id ("" never matches).
    // Returns a pointer into the static list, or nullptr. Never throws.
    inline const LaunchPresetTemplate* FindLaunchPresetTemplate(std::wstring_view id) noexcept
    {
        if (id.empty())
        {
            return nullptr;
        }
        for (const auto& t : LaunchPresetTemplates())
        {
            if (t.id == id)
            {
                return &t;
            }
        }
        return nullptr;
    }

    // The preset with `id` in `cfg`.
    //   cfg - the config.
    //   id  - the preset id.
    // Returns a pointer into cfg.presets (invalidated by any change to the vector), or nullptr.
    inline const LaunchPreset* FindLaunchPreset(const LaunchPresetsConfig& cfg, std::wstring_view id) noexcept
    {
        for (const auto& p : cfg.presets)
        {
            if (p.id == id)
            {
                return &p;
            }
        }
        return nullptr;
    }

    // A fresh id for a user preset ('p') or a custom provider ('c'): "<prefix>-<8 hex>". The caller supplies
    // the entropy (the OS half passes a GUID hash), and the id is re-drawn while it collides with one in use.
    //   prefix  - 'p' or 'c'.
    //   entropy - any 64-bit value; different values give different ids.
    //   inUse   - returns true for an id that is already taken.
    // Returns the id.
    inline std::wstring MakeLaunchPresetId(wchar_t prefix, uint64_t entropy, const std::function<bool(const std::wstring&)>& inUse)
    {
        std::wstring id;
        // Bounded: 32 bits of id space against a list of a few dozen presets collide almost never, and a
        // caller whose inUse() always answers true must still get an id rather than hang the UI thread.
        for (uint64_t salt = 0; salt < 4096; ++salt)
        {
            const uint64_t h = detail::PresetFnv1a(std::to_wstring(entropy) + L":" + std::to_wstring(salt));
            id = std::wstring(1, prefix) + L"-" + detail::PresetHex(h, 8);
            if (!inUse || !inUse(id))
            {
                return id;
            }
        }
        return id;
    }

    // ===== templates in the user's list ======================================================

    // True when `cfg` holds a preset made from the template: one LINKED to it (LaunchPreset::templateId), whatever
    // its id — a preset added in Settings ("p-<8 hex>") or one release 0.6.8.18 added by itself (its id the
    // template's). Settings ▸ Presets ▸ TEMPLATES shows such a template as "Added".
    //   cfg        - the presets config.
    //   templateId - the template id ("" never matches).
    // Never throws.
    inline bool LaunchPresetListHasTemplate(const LaunchPresetsConfig& cfg, std::wstring_view templateId) noexcept
    {
        if (templateId.empty())
        {
            return false;
        }
        for (const auto& p : cfg.presets)
        {
            if (p.templateId == templateId)
            {
                return true;
            }
        }
        return false;
    }

    // The shipped templates that are NOT in the user's list yet (LaunchPresetListHasTemplate), in the shipped order:
    // what "Add all tested templates" adds, and what decides whether that button is enabled.
    //   cfg        - the presets config.
    //   testedOnly - true = only the templates with a `tested` line (an untested one is still added by its own row).
    // Returns template ids (empty when every template asked for is in the list).
    inline std::vector<std::wstring> LaunchPresetTemplatesNotInList(const LaunchPresetsConfig& cfg, bool testedOnly)
    {
        std::vector<std::wstring> out;
        for (const auto& t : LaunchPresetTemplates())
        {
            if ((testedOnly && t.tested.empty()) || LaunchPresetListHasTemplate(cfg, t.id))
            {
                continue;
            }
            out.push_back(t.id);
        }
        return out;
    }

    // Add one preset per template at the END of the list (menu order). Each new preset is LINKED to its template
    // and has no typed field, so every value follows the template (its name too); it is visible in the launch
    // menus, an untested template's preset included (the user added it on purpose, and Settings says "not tested
    // yet" beside it); and it gets a fresh "p-<8 hex>" id (MakeLaunchPresetId). A template that is already in the
    // list is added AGAIN, as a second linked preset: the caller decides (Settings offers Add only for a template
    // that is not in the list). An unknown template id is skipped.
    //   cfg         - the presets config (modified).
    //   templateIds - the template ids, in the order to add them.
    //   entropy     - seeds the new ids (the OS half passes NewPresetEntropy()).
    // Returns the ids of the added presets, in order (empty when nothing was added).
    inline std::vector<std::wstring> AddLaunchPresetsFromTemplates(LaunchPresetsConfig& cfg, const std::vector<std::wstring>& templateIds, uint64_t entropy)
    {
        std::vector<std::wstring> added;
        for (const auto& templateId : templateIds)
        {
            if (!FindLaunchPresetTemplate(templateId))
            {
                continue;
            }
            LaunchPreset p;
            // One entropy value serves the whole batch: each new id is in the list before the next one is drawn,
            // so the collision check re-draws (the next salt) instead of handing out the same id twice.
            p.id = MakeLaunchPresetId(L'p', entropy, [&cfg](const std::wstring& id) { return FindLaunchPreset(cfg, id) != nullptr; });
            p.templateId = templateId;
            added.push_back(p.id);
            cfg.presets.push_back(std::move(p));
        }
        return added;
    }

    // The one-time cleanup of a launch-presets.json written by release 0.6.8.18 (82cd65d2a): the one build that put
    // the shipped templates into the list BY ITSELF ("seeding") and recorded their ids under "seededTemplates".
    // Since 2026-10-06 the list starts empty and only the user adds templates (the user's rule), so a preset that is
    // still EXACTLY what that build added is left out. All of these must hold: its id IS its template's id (the
    // seeding mark — a preset added in Settings gets a "p-" id), the file lists that id under seededTemplates, it
    // links to a shipped template, no field was typed into it, and its menu switch is where seeding left it (hidden
    // exactly when the template is untested — that build's rule). A preset the user touched in any of these ways
    // stays. Sessions that started on a dropped preset keep their own snapshot (launchArgs), so nothing that exists
    // changes, and the next Settings Save writes the file without the dropped presets (and without the list).
    //   cfg          - the parsed config (modified).
    //   autoAddedIds - the file's "seededTemplates" list (empty = a file this build wrote: nothing is dropped).
    // Returns the ids of the dropped presets, in list order.
    inline std::vector<std::wstring> DropUntouchedAutoAddedPresets(LaunchPresetsConfig& cfg, const std::vector<std::wstring>& autoAddedIds)
    {
        std::vector<std::wstring> dropped;
        if (autoAddedIds.empty())
        {
            return dropped;
        }
        std::vector<LaunchPreset> kept;
        kept.reserve(cfg.presets.size());
        for (auto& p : cfg.presets)
        {
            const auto* t = FindLaunchPresetTemplate(p.templateId);
            const bool untouched = t && p.id == p.templateId && p.fields.empty() && p.hidden == t->tested.empty() &&
                                   std::find(autoAddedIds.begin(), autoAddedIds.end(), p.id) != autoAddedIds.end();
            if (untouched)
            {
                dropped.push_back(p.id);
            }
            else
            {
                kept.push_back(std::move(p));
            }
        }
        cfg.presets = std::move(kept);
        return dropped;
    }

    // A provider id resolved against the built-ins and the user's entries.
    //   cfg - the config (for overrides and custom providers).
    //   id  - the provider id ("" and kAnthropicProviderId resolve to "not found": the Claude login).
    // Returns the merged provider; found = false for an unknown id.
    inline ResolvedLaunchProvider ResolveLaunchProvider(const LaunchPresetsConfig& cfg, std::wstring_view id)
    {
        ResolvedLaunchProvider r;
        if (id.empty() || id == kAnthropicProviderId)
        {
            return r;
        }
        const LaunchProviderEntry* entry = nullptr;
        for (const auto& e : cfg.providers)
        {
            if (e.id == id)
            {
                entry = &e;
                break;
            }
        }
        for (const auto& d : kBuiltInLaunchProviders)
        {
            if (id == d.id)
            {
                r.found = true;
                r.routing = d.routing;
                r.id = d.id;
                r.name = d.name;
                r.envPrefix = d.envPrefix;
                r.keyHint = d.keyHint;
                r.baseUrlDefault = d.anthropicBaseUrl;
                r.baseUrlBox = entry ? detail::PresetTrim(entry->anthropicBaseUrl) : std::wstring{};
                r.baseUrlExample = d.baseUrlExample;
                r.modelRequired = d.modelRequired;
                return r;
            }
        }
        if (entry && entry->custom)
        {
            r.found = true;
            r.custom = true;
            r.id = entry->id;
            r.name = entry->name.empty() ? entry->id : entry->name;
            r.envPrefix = entry->envPrefix;
            r.baseUrlBox = detail::PresetTrim(entry->anthropicBaseUrl);
        }
        return r;
    }

    // An env prefix sanitized for use: ASCII upper case, [A-Z0-9_] only, no leading digit, no trailing '_'.
    // A custom provider's prefix comes from a text box, and "my-glm" must not produce MY-GLM_API_KEY.
    //   prefix - the typed prefix.
    // Returns the sanitized prefix ("" when nothing usable remains).
    inline std::wstring SanitizeLaunchEnvPrefix(std::wstring_view prefix)
    {
        std::wstring out;
        for (const wchar_t c : prefix)
        {
            if (c >= L'a' && c <= L'z')
            {
                out.push_back(static_cast<wchar_t>(c - L'a' + L'A'));
            }
            else if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_')
            {
                out.push_back(c);
            }
            else if (c == L'-' || c == L' ')
            {
                out.push_back(L'_');
            }
        }
        while (!out.empty() && out.back() == L'_')
        {
            out.pop_back();
        }
        while (!out.empty() && ((out.front() >= L'0' && out.front() <= L'9') || out.front() == L'_'))
        {
            out.erase(out.begin());
        }
        return out;
    }

    // ===== field table ======================================================================

    // How a field is edited and validated.
    enum class PresetFieldKind : uint8_t
    {
        Text, // free text (model ids, a name)
        Provider, // a provider id (a combo box in the UI)
        Integer, // ASCII digits only (token limits)
        Decimal, // a decimal number (sampling)
        List, // a comma/space separated list
        Boolean, // "true" / "false" / empty
        Effort, // low / medium / high / xhigh / max
        Json, // a JSON object
        EnvLines, // NAME=VALUE per line
        Multiline, // free multi-line text (the identity note)
        Arguments // raw claude arguments
    };

    // When a field applies. A field outside its scope is not shown in the editor and not compiled.
    enum class PresetFieldScope : uint8_t
    {
        Always, // every preset, the Claude login included
        ProviderOnly, // only a preset with a provider (the Claude login has its own models and limits)
        RoutingOnly // only a provider with OpenRouter's routing block
    };

    // One row of the field table.
    struct PresetFieldSpec
    {
        const wchar_t* key; // the field key (kPresetField*)
        const wchar_t* label; // the editor's header
        const wchar_t* envSuffix; // <PREFIX>_<suffix> is the field's env variable; nullptr = no env tier
        PresetFieldKind kind;
        PresetFieldScope scope;
        const wchar_t* tip; // the editor's hover description
    };

    // The fields in editor order. Name and Provider lead; everything below them is a launch value.
    inline constexpr PresetFieldSpec kPresetFieldSpecs[] = {
        { kPresetFieldName, L"Name", nullptr, PresetFieldKind::Text, PresetFieldScope::Always,
          L"The name the launch menus show for this preset. Renaming it never breaks a session that already uses the preset." },
        { kPresetFieldProvider, L"Provider", nullptr, PresetFieldKind::Provider, PresetFieldScope::Always,
          L"Where the session's requests go. \"Claude login\" keeps your own Claude account; OpenRouter, NanoGPT, Xiaomi MiMo, RunPod or a custom provider replace it for this session only." },
        { kPresetFieldModel, L"Main model", nullptr, PresetFieldKind::Text, PresetFieldScope::Always,
          L"The model the session runs on. With a provider, every tier (Fable, Opus, Sonnet and subagents) uses it unless a field below says otherwise. With your Claude login it is passed as --model. On RunPod, type the Hugging Face id your server serves (vLLM's served model name): a RunPod preset without a model does not launch." },
        { kPresetFieldSmallModel, L"Background model", L"SMALL_MODEL", PresetFieldKind::Text, PresetFieldScope::ProviderOnly,
          L"The model for the haiku tier and background work (titles, WebFetch summaries). Empty = the main model." },
        { kPresetFieldSwitchModel, L"/model switch entry", L"SWITCH_MODEL", PresetFieldKind::Text, PresetFieldScope::ProviderOnly,
          L"An extra entry in Claude Code's /model picker, so you can move the session to another model mid-way. Type none for no entry." },
        { kPresetFieldEffort, L"Effort", nullptr, PresetFieldKind::Effort, PresetFieldScope::Always,
          L"The session's reasoning effort: low, medium, high, xhigh or max. Written as CLAUDE_CODE_EFFORT_LEVEL into the session's settings file (Claude Code obeys that variable over --effort and over the same variable in ~/.claude/settings.json) and passed as --effort. While it is set, /effort in the session cannot change the level. Type none for no effort setting; empty = the template's, else Claude Code's own setting." },
        { kPresetFieldContext, L"Context window (tokens)", L"CONTEXT", PresetFieldKind::Integer, PresetFieldScope::ProviderOnly,
          L"CLAUDE_CODE_MAX_CONTEXT_TOKENS: the window Claude Code plans compaction around. Use the SMALLEST window among the providers that may serve the model. On your own vLLM server (RunPod), use its --max-model-len MINUS Max output tokens: vLLM refuses a request whose input plus max_tokens passes its window." },
        { kPresetFieldMaxOutput, L"Max output tokens", L"MAX_OUTPUT", PresetFieldKind::Integer, PresetFieldScope::Always,
          L"CLAUDE_CODE_MAX_OUTPUT_TOKENS. Claude Code caps an unrecognized model at 128,000 whatever this says. A RunPod Serverless endpoint ends any request after 5.5 minutes, so keep one answer short enough to finish in that time." },
        { kPresetFieldProviderOrder, L"Provider order", L"PROVIDER", PresetFieldKind::List, PresetFieldScope::RoutingOnly,
          L"OpenRouter's provider.order: the hosts to try, first to last (comma separated)." },
        { kPresetFieldQuantizations, L"Quantizations", nullptr, PresetFieldKind::List, PresetFieldScope::RoutingOnly,
          L"OpenRouter's provider.quantizations: only hosts serving these weights may answer (bf16, fp8, mxfp4, ...). A fallback can never reach a more heavily quantized host. A host that does not state its precision is dropped too." },
        { kPresetFieldRequireParameters, L"Require parameter support", nullptr, PresetFieldKind::Boolean, PresetFieldScope::RoutingOnly,
          L"OpenRouter's provider.require_parameters: true = only hosts that support every parameter of the request may answer, so a host that would silently ignore one (top_k, for example) is skipped. Empty = OpenRouter's default (false)." },
        { kPresetFieldAllowFallbacks, L"Allow fallbacks", nullptr, PresetFieldKind::Boolean, PresetFieldScope::RoutingOnly,
          L"OpenRouter's provider.allow_fallbacks: false = only the hosts in the order above, or an error. Empty = OpenRouter's default (true)." },
        { kPresetFieldTemperature, L"Temperature", nullptr, PresetFieldKind::Decimal, PresetFieldScope::ProviderOnly,
          L"Sent in every request body. Use the model vendor's recommended value (1.0 for MiMo, Kimi K3, Qwen3.8 and GLM-5.3); without an explicit value a third-party host applies its own default." },
        { kPresetFieldTopP, L"top_p", nullptr, PresetFieldKind::Decimal, PresetFieldScope::ProviderOnly,
          L"Sent in every request body. Vendors' values: 0.95 for MiMo and Qwen3.8, 1.0 for Kimi K3 and GLM-5.3." },
        { kPresetFieldTopK, L"top_k", nullptr, PresetFieldKind::Integer, PresetFieldScope::ProviderOnly,
          L"Sent in every request body, as a whole number (Qwen recommends 20 for Qwen3.8). Empty = not sent. Not every host honors it: on OpenRouter, set Require parameter support to true so such a host is skipped." },
        { kPresetFieldExtraBody, L"Extra request body (JSON)", L"EXTRA_BODY", PresetFieldKind::Json, PresetFieldScope::ProviderOnly,
          L"A JSON object merged over the fields above into every request (CLAUDE_CODE_EXTRA_BODY). Objects merge; any other value replaces." },
        { kPresetFieldExtraEnv, L"Extra env vars", nullptr, PresetFieldKind::EnvLines, PresetFieldScope::Always,
          L"NAME=VALUE per line, written into the session's settings file, so they win over ~/.claude/settings.json for this session. CCMGR_*, AM_* and WT_SESSION are reserved." },
        { kPresetFieldIdentityNote, L"Identity note", nullptr, PresetFieldKind::Multiline, PresetFieldScope::ProviderOnly,
          L"Optional; empty = nothing is appended. Text appended to the system prompt (--append-system-prompt). Claude Code tells an unknown model that it is Claude; a note such as \"This session runs on {model} through {provider}.\" corrects that. {model}, {provider}, {switchModel} and {switch} are filled in." },
        { kPresetFieldExtraArgs, L"Extra claude arguments", nullptr, PresetFieldKind::Arguments, PresetFieldScope::Always,
          L"Appended to the claude command line, for example --autocompact 600000 or --permission-mode plan. Agentmaster owns --settings, --session-id, --resume, --fork-session, --model and print mode." },
    };

    // The table row for `key`.
    //   key - a field key.
    // Returns a pointer into kPresetFieldSpecs, or nullptr. Never throws.
    inline const PresetFieldSpec* FindPresetFieldSpec(std::wstring_view key) noexcept
    {
        for (const auto& s : kPresetFieldSpecs)
        {
            if (key == s.key)
            {
                return &s;
            }
        }
        return nullptr;
    }

    // True when `scope` applies to a preset whose provider is `provider` (found = a real provider).
    //   scope    - the field's scope.
    //   provider - the preset's resolved provider.
    // Never throws.
    inline bool PresetFieldInScope(PresetFieldScope scope, const ResolvedLaunchProvider& provider) noexcept
    {
        switch (scope)
        {
        case PresetFieldScope::Always:
            return true;
        case PresetFieldScope::ProviderOnly:
            return provider.found;
        case PresetFieldScope::RoutingOnly:
            return provider.found && provider.routing;
        }
        return false;
    }

    // ===== resolution =======================================================================

    // Reads one environment variable; "" when absent. The OS half supplies two: Agentmaster's env list
    // (Settings, Global + Per-directory) and the Windows environment.
    using PresetEnvLookup = std::function<std::wstring(std::wstring_view name)>;

    // Where a resolved value came from — drives the gray placeholder's "← source" label.
    enum class PresetValueSource : uint8_t
    {
        None, // nothing set anywhere
        Box, // typed in the preset
        AgentmasterEnv, // Agentmaster's env list (Settings)
        WindowsEnv, // the Windows environment
        Template, // the linked template
        ProviderDefault, // the built-in provider's default (the Base URL)
        Stored // the Windows Credential Manager (keys only)
    };

    // One resolved value.
    struct PresetFieldValue
    {
        std::wstring value; // "" when nothing is set
        PresetValueSource source{ PresetValueSource::None };
        std::wstring envName; // the env variable this field reads ("" when it has no env tier)
    };

    // The short source label a placeholder shows after the arrow.
    //   v - the resolved value.
    // Returns e.g. "OPENROUTER_SMALL_MODEL (Windows)", "template", "" for None.
    inline std::wstring PresetValueSourceLabel(const PresetFieldValue& v)
    {
        switch (v.source)
        {
        case PresetValueSource::None:
            return {};
        case PresetValueSource::Box:
            return L"this box";
        case PresetValueSource::AgentmasterEnv:
            return v.envName + L" (Agentmaster env)";
        case PresetValueSource::WindowsEnv:
            return v.envName + L" (Windows)";
        case PresetValueSource::Template:
            return L"template";
        case PresetValueSource::ProviderDefault:
            return L"provider default";
        case PresetValueSource::Stored:
            return L"Windows Credential Manager";
        }
        return {};
    }

    // A preset's effective provider id: its box, else its template's, else "" (the Claude login). The
    // explicit kAnthropicProviderId in the box also resolves to "" — that is how a user moves a
    // template-linked preset onto the Claude login.
    //   preset - the preset.
    // Returns the provider id, or "" for the Claude login.
    inline std::wstring PresetProviderId(const LaunchPreset& preset)
    {
        std::wstring id;
        if (const auto it = preset.fields.find(kPresetFieldProvider); it != preset.fields.end())
        {
            id = detail::PresetTrim(it->second);
        }
        if (id.empty())
        {
            if (const auto* t = FindLaunchPresetTemplate(preset.templateId))
            {
                if (const auto it = t->fields.find(kPresetFieldProvider); it != t->fields.end())
                {
                    id = it->second;
                }
            }
        }
        return id == kAnthropicProviderId ? std::wstring{} : id;
    }

    // A preset's display name: its box, else its template's, else its id.
    //   preset - the preset.
    // Returns the name (never empty for a preset with an id).
    inline std::wstring PresetDisplayName(const LaunchPreset& preset)
    {
        if (const auto it = preset.fields.find(kPresetFieldName); it != preset.fields.end())
        {
            if (auto n = detail::PresetTrim(it->second); !n.empty())
            {
                return n;
            }
        }
        if (const auto* t = FindLaunchPresetTemplate(preset.templateId))
        {
            if (const auto it = t->fields.find(kPresetFieldName); it != t->fields.end())
            {
                return it->second;
            }
        }
        return preset.id;
    }

    // Resolve one preset field through the tiers (box, Agentmaster env, Windows env, template).
    //   key            - the field key (Name and Provider resolve box -> template only).
    //   preset         - the preset.
    //   cfg            - the config (for the provider's env prefix).
    //   agentmasterEnv - Agentmaster's env list lookup (may be empty: tier skipped).
    //   windowsEnv     - the Windows environment lookup (may be empty: tier skipped).
    //   includeBox     - false = start below the box: what a placeholder shows while the box is empty.
    // Returns the value + its source; envName is set whenever the field has an env tier for this preset.
    inline PresetFieldValue ResolvePresetField(std::wstring_view key,
                                               const LaunchPreset& preset,
                                               const LaunchPresetsConfig& cfg,
                                               const PresetEnvLookup& agentmasterEnv,
                                               const PresetEnvLookup& windowsEnv,
                                               bool includeBox)
    {
        PresetFieldValue out;
        const auto* spec = FindPresetFieldSpec(key);
        const auto provider = ResolveLaunchProvider(cfg, PresetProviderId(preset));
        if (spec && spec->envSuffix && provider.found && !provider.envPrefix.empty())
        {
            out.envName = provider.envPrefix + L"_" + spec->envSuffix;
        }
        if (includeBox)
        {
            if (const auto it = preset.fields.find(std::wstring{ key }); it != preset.fields.end())
            {
                if (auto v = detail::PresetTrim(it->second); !v.empty())
                {
                    out.value = std::move(v);
                    out.source = PresetValueSource::Box;
                    return out;
                }
            }
        }
        if (!out.envName.empty())
        {
            if (agentmasterEnv)
            {
                if (auto v = detail::PresetTrim(agentmasterEnv(out.envName)); !v.empty())
                {
                    out.value = std::move(v);
                    out.source = PresetValueSource::AgentmasterEnv;
                    return out;
                }
            }
            if (windowsEnv)
            {
                if (auto v = detail::PresetTrim(windowsEnv(out.envName)); !v.empty())
                {
                    out.value = std::move(v);
                    out.source = PresetValueSource::WindowsEnv;
                    return out;
                }
            }
        }
        if (const auto* t = FindLaunchPresetTemplate(preset.templateId))
        {
            if (const auto it = t->fields.find(std::wstring{ key }); it != t->fields.end() && !it->second.empty())
            {
                out.value = it->second;
                out.source = PresetValueSource::Template;
            }
        }
        return out;
    }

    // Resolve a provider's Base URL through the tiers (box, Agentmaster env, Windows env, default).
    //   provider       - the resolved provider.
    //   agentmasterEnv - Agentmaster's env list lookup (may be empty).
    //   windowsEnv     - the Windows environment lookup (may be empty).
    //   includeBox     - false = what the placeholder shows while the box is empty.
    // Returns the URL + its source ("" / None for an unknown provider or a custom one with nothing set).
    inline PresetFieldValue ResolveProviderBaseUrl(const ResolvedLaunchProvider& provider,
                                                   const PresetEnvLookup& agentmasterEnv,
                                                   const PresetEnvLookup& windowsEnv,
                                                   bool includeBox)
    {
        PresetFieldValue out;
        if (!provider.found)
        {
            return out;
        }
        if (!provider.envPrefix.empty())
        {
            out.envName = provider.envPrefix + L"_BASE_URL";
        }
        if (includeBox && !provider.baseUrlBox.empty())
        {
            out.value = provider.baseUrlBox;
            out.source = PresetValueSource::Box;
            return out;
        }
        if (!out.envName.empty())
        {
            if (agentmasterEnv)
            {
                if (auto v = detail::PresetTrim(agentmasterEnv(out.envName)); !v.empty())
                {
                    out.value = std::move(v);
                    out.source = PresetValueSource::AgentmasterEnv;
                    return out;
                }
            }
            if (windowsEnv)
            {
                if (auto v = detail::PresetTrim(windowsEnv(out.envName)); !v.empty())
                {
                    out.value = std::move(v);
                    out.source = PresetValueSource::WindowsEnv;
                    return out;
                }
            }
        }
        if (!provider.baseUrlDefault.empty())
        {
            out.value = provider.baseUrlDefault;
            out.source = PresetValueSource::ProviderDefault;
        }
        return out;
    }

    // ===== the per-session snapshot ==========================================================

    // The flattened launch configuration a session keeps for life (see the header comment's tiers).
    struct LaunchArgs
    {
        uint32_t version{ 1 }; // the JSON schema version
        std::wstring agent{ L"claude" }; // "claude" today; "codex" when Codex presets exist
        std::wstring presetId; // provenance only ("" for a plain model pick); never looked up again
        std::wstring presetName; // the preset's name at snapshot time (display and logs)
        std::wstring providerId; // "" = the user's Claude login
        std::wstring providerName; // the provider's name at snapshot time (the /model display names)
        bool routing{ false }; // the provider takes OpenRouter's routing block
        std::map<std::wstring, std::wstring> set; // tiers 1 + 2: the values that always win
        std::map<std::wstring, std::wstring> env; // tier 3: field key -> Windows variable name, re-read per launch
        std::map<std::wstring, std::wstring> base; // tier 4: the values used when the tier-3 variable is absent
        std::wstring keyCredential; // the provider id whose Credential Manager entry holds the key ("" = no key)
        std::wstring keyEnv; // the key's variable name, looked up in Agentmaster's env, then in Windows
    };

    // The snapshot a PLAIN model pick takes ("Open New Session Here ▸ Fable"): just the model, so a resume
    // keeps the model the session started on.
    //   model - the picked model id.
    // Returns the snapshot.
    inline LaunchArgs SnapshotModelLaunch(std::wstring_view model)
    {
        LaunchArgs a;
        if (auto m = detail::PresetTrim(model); !m.empty())
        {
            a.set[kPresetFieldModel] = std::move(m);
        }
        return a;
    }

    // The snapshot a preset launch takes at the session's FIRST launch. Tiers 1 and 2 are frozen into `set`;
    // for every field with an env tier and no frozen value, the Windows variable NAME goes into `env` and the
    // template's value into `base`.
    //   preset         - the preset.
    //   cfg            - the config (provider entries).
    //   agentmasterEnv - Agentmaster's env list for the launch directory (Global + Per-directory).
    // Returns the snapshot.
    inline LaunchArgs SnapshotPresetLaunch(const LaunchPreset& preset, const LaunchPresetsConfig& cfg, const PresetEnvLookup& agentmasterEnv)
    {
        LaunchArgs a;
        a.presetId = preset.id;
        a.presetName = PresetDisplayName(preset);
        const auto provider = ResolveLaunchProvider(cfg, PresetProviderId(preset));
        if (provider.found)
        {
            a.providerId = provider.id;
            a.providerName = provider.name;
            a.routing = provider.routing;
            a.keyCredential = provider.id;
            if (!provider.envPrefix.empty())
            {
                a.keyEnv = provider.envPrefix + L"_API_KEY";
            }
            // The Base URL lives on the provider, but the session keeps the URL it started with.
            const auto frozen = ResolveProviderBaseUrl(provider, agentmasterEnv, nullptr, true);
            if (frozen.source == PresetValueSource::Box || frozen.source == PresetValueSource::AgentmasterEnv)
            {
                a.set[kPresetFieldBaseUrl] = frozen.value;
            }
            else
            {
                if (!frozen.envName.empty())
                {
                    a.env[kPresetFieldBaseUrl] = frozen.envName;
                }
                if (!provider.baseUrlDefault.empty())
                {
                    a.base[kPresetFieldBaseUrl] = provider.baseUrlDefault;
                }
            }
        }
        for (const auto& spec : kPresetFieldSpecs)
        {
            if (spec.kind == PresetFieldKind::Provider || std::wstring_view{ spec.key } == kPresetFieldName || !PresetFieldInScope(spec.scope, provider))
            {
                continue; // name/provider are provenance, not launch values; out-of-scope fields never compile
            }
            // Tiers 1-2 (Windows deliberately not consulted: its NAME is recorded instead, re-read each launch).
            const auto v = ResolvePresetField(spec.key, preset, cfg, agentmasterEnv, nullptr, true);
            if (v.source == PresetValueSource::Box || v.source == PresetValueSource::AgentmasterEnv)
            {
                a.set[spec.key] = v.value;
                continue;
            }
            if (!v.envName.empty())
            {
                a.env[spec.key] = v.envName;
            }
            if (v.source == PresetValueSource::Template)
            {
                a.base[spec.key] = v.value;
            }
        }
        return a;
    }

    // Why a preset cannot launch at all. Checked when the preset is PICKED, before any snapshot:
    //   * its provider no longer exists (a custom provider removed in Settings). SnapshotPresetLaunch would then
    //     record no provider, and the session would start on the user's own Claude login with the preset's
    //     model - a silent switch to another account and another bill;
    //   * its provider serves one model only (LaunchProviderDef::modelRequired: RunPod) and the preset names
    //     none. Claude Code would then ask the server for its own Claude model ids, and every request would
    //     fail with "model does not exist". The Main model has no env tier, so the box and the template decide.
    // A session that already holds a snapshot is not affected: its provider, Base URL, key reference and model
    // are frozen in the snapshot.
    //   preset - the preset.
    //   cfg    - the config (its providers).
    // Returns the user-facing reason, or "" when the preset can launch (the Claude login, a built-in
    // provider, or a custom provider that still exists, with a model where its provider needs one).
    inline std::wstring PresetLaunchRefusal(const LaunchPreset& preset, const LaunchPresetsConfig& cfg)
    {
        const auto providerId = PresetProviderId(preset);
        if (providerId.empty())
        {
            return {};
        }
        const auto provider = ResolveLaunchProvider(cfg, providerId);
        if (!provider.found)
        {
            return L"The preset \"" + PresetDisplayName(preset) + L"\" uses the provider \"" + providerId +
                   L"\", which no longer exists. Pick another provider for it in Settings \x25B8 Presets.";
        }
        // No env lookups: the Main model has no env tier (its spec's envSuffix is nullptr).
        if (provider.modelRequired && ResolvePresetField(kPresetFieldModel, preset, cfg, nullptr, nullptr, true).value.empty())
        {
            return L"The preset \"" + PresetDisplayName(preset) + L"\" names no model, and " + provider.name +
                   L" serves only the model its server runs. Type that model's Hugging Face id (vLLM's served model name) into the preset's Main model box in Settings \x25B8 Presets.";
        }
        return {};
    }

    // The values a session launches with NOW: `set`, else the Windows variable named in `env` when it is
    // set at this moment, else `base`.
    //   a          - the snapshot.
    //   windowsEnv - the Windows environment lookup (may be empty: the env tier is skipped).
    // Returns field key -> value (only keys with a non-empty value).
    inline std::map<std::wstring, std::wstring> EffectiveLaunchValues(const LaunchArgs& a, const PresetEnvLookup& windowsEnv)
    {
        std::map<std::wstring, std::wstring> out;
        for (const auto& [k, v] : a.set)
        {
            if (!v.empty())
            {
                out[k] = v;
            }
        }
        const auto fill = [&](const std::wstring& k, const std::wstring& v) {
            if (!v.empty() && out.find(k) == out.end())
            {
                out[k] = v;
            }
        };
        for (const auto& [k, name] : a.env)
        {
            if (windowsEnv && out.find(k) == out.end())
            {
                fill(k, detail::PresetTrim(windowsEnv(name)));
            }
        }
        for (const auto& [k, v] : a.base)
        {
            fill(k, v);
        }
        return out;
    }

    // Serialize a snapshot for the session store (one compact JSON string).
    //   a - the snapshot.
    // Returns the JSON text.
    inline std::wstring LaunchArgsToJson(const LaunchArgs& a)
    {
        json::Value o = json::Value::MkObj();
        o.Set(L"v", json::Value::MkNum(static_cast<double>(a.version)));
        o.Set(L"agent", json::Value::MkStr(a.agent));
        if (!a.presetId.empty())
        {
            o.Set(L"preset", json::Value::MkStr(a.presetId));
            o.Set(L"presetName", json::Value::MkStr(a.presetName));
        }
        if (!a.providerId.empty())
        {
            o.Set(L"provider", json::Value::MkStr(a.providerId));
            o.Set(L"providerName", json::Value::MkStr(a.providerName));
            o.Set(L"routing", json::Value::MkBool(a.routing));
        }
        const auto putMap = [&](const wchar_t* name, const std::map<std::wstring, std::wstring>& m) {
            if (m.empty())
            {
                return;
            }
            json::Value obj = json::Value::MkObj();
            for (const auto& [k, v] : m)
            {
                obj.Set(k, json::Value::MkStr(v));
            }
            o.Set(name, std::move(obj));
        };
        putMap(L"set", a.set);
        putMap(L"env", a.env);
        putMap(L"base", a.base);
        if (!a.keyCredential.empty() || !a.keyEnv.empty())
        {
            json::Value key = json::Value::MkObj();
            key.Set(L"credential", json::Value::MkStr(a.keyCredential));
            key.Set(L"env", json::Value::MkStr(a.keyEnv));
            o.Set(L"key", std::move(key));
        }
        return json::Dump(o);
    }

    // Parse a stored snapshot.
    //   text - the session-store value.
    // Returns the snapshot, or nullopt for empty / malformed text, a non-object, an unknown agent, or a
    // newer schema version this build cannot read (better Default than a misread launch).
    inline std::optional<LaunchArgs> LaunchArgsFromJson(std::wstring_view text)
    {
        if (detail::PresetTrim(text).empty())
        {
            return std::nullopt;
        }
        const auto parsed = json::Parse(text);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        const auto& o = *parsed;
        LaunchArgs a;
        a.version = o.U32At(L"v", 1);
        if (a.version == 0 || a.version > 1)
        {
            return std::nullopt;
        }
        a.agent = o.StrAt(L"agent", L"claude");
        if (a.agent != L"claude")
        {
            return std::nullopt; // a Codex snapshot from a later build: not launchable here
        }
        a.presetId = o.StrAt(L"preset");
        a.presetName = o.StrAt(L"presetName");
        a.providerId = o.StrAt(L"provider");
        a.providerName = o.StrAt(L"providerName");
        a.routing = o.BoolAt(L"routing");
        const auto readMap = [&](const wchar_t* name, std::map<std::wstring, std::wstring>& m) {
            if (const auto* obj = o.Find(name); obj && obj->type == json::Value::Type::Obj)
            {
                for (const auto& [k, v] : obj->members)
                {
                    if (v.type == json::Value::Type::Str)
                    {
                        m[k] = v.str;
                    }
                }
            }
        };
        readMap(L"set", a.set);
        readMap(L"env", a.env);
        readMap(L"base", a.base);
        if (const auto* key = o.Find(L"key"); key && key->type == json::Value::Type::Obj)
        {
            a.keyCredential = key->StrAt(L"credential");
            a.keyEnv = key->StrAt(L"env");
        }
        return a;
    }

    // ===== the Claude compile step ===========================================================

    // What one launch adds to the plain Claude spawn (ClaudeSpawn.cpp consumes it).
    struct CompiledClaudeLaunch
    {
        std::wstring presetId; // for logs ("" for a plain model pick)
        std::wstring presetName;
        std::wstring providerId; // "" = the Claude login
        std::wstring providerName;
        std::wstring model; // --model, and the settings file's "model" ("" = leave both alone)
        std::wstring effort; // --effort ("" = none); settingsEnv carries the same level as CLAUDE_CODE_EFFORT_LEVEL
        std::wstring appendSystemPrompt; // --append-system-prompt ("" = none)
        std::wstring extraArgs; // raw claude arguments, appended before any positional prompt
        std::vector<std::pair<std::wstring, std::wstring>> settingsEnv; // the settings file's env block, in order
        std::vector<std::pair<std::wstring, std::wstring>> processEnv; // AM_PRESET + the token variable
        std::wstring authTokenEnv; // the process variable the pwsh host moves into ANTHROPIC_AUTH_TOKEN ("" = none)
        bool sessionSettings{ false }; // true = write a launch settings file (hooks + settingsEnv)
        std::vector<std::wstring> warnings; // values dropped as invalid (the preview + the [preset] log line)
        std::wstring error; // non-empty = do not launch (no key / no Base URL)
    };

    // Compose the request-body extras for a provider: OpenRouter's routing block, the sampling fields, then
    // the extra-body JSON deep-merged over them. The key order (provider: order, quantizations,
    // require_parameters, allow_fallbacks; then temperature, top_p, top_k) is the reference profiles' own, so a
    // template's body reads like the launcher's; JSON readers do not depend on it.
    //   values   - the effective values (EffectiveLaunchValues).
    //   routing  - the provider takes OpenRouter's routing block.
    //   warnings - receives a note per value dropped as invalid.
    // Returns the body as an object value (no members = nothing to send).
    inline json::Value ComposePresetExtraBody(const std::map<std::wstring, std::wstring>& values, bool routing, std::vector<std::wstring>& warnings)
    {
        const auto get = [&](const wchar_t* k) -> std::wstring {
            const auto it = values.find(k);
            return it == values.end() ? std::wstring{} : detail::PresetTrim(it->second);
        };
        json::Value body = json::Value::MkObj();
        if (routing)
        {
            json::Value prov = json::Value::MkObj();
            const auto putList = [&](const wchar_t* field, const wchar_t* jsonKey) {
                const auto items = detail::PresetSplitList(get(field));
                if (items.empty())
                {
                    return;
                }
                json::Value arr = json::Value::MkArr();
                for (const auto& item : items)
                {
                    arr.Push(json::Value::MkStr(item));
                }
                prov.Set(jsonKey, std::move(arr));
            };
            // A true/false routing switch. Anything else is dropped with a warning rather than guessed: a
            // misread "allow_fallbacks" would let a request reach a host the preset excludes.
            const auto putBool = [&](const wchar_t* field, const wchar_t* jsonKey, const wchar_t* label) {
                const auto text = get(field);
                if (text.empty())
                {
                    return;
                }
                if (detail::PresetEqualsCi(text, L"true") || detail::PresetEqualsCi(text, L"false"))
                {
                    prov.Set(jsonKey, json::Value::MkBool(detail::PresetEqualsCi(text, L"true")));
                }
                else
                {
                    warnings.push_back(std::wstring{ label } + L": \"" + text + L"\" is not true or false (not sent)");
                }
            };
            putList(kPresetFieldProviderOrder, L"order");
            putList(kPresetFieldQuantizations, L"quantizations");
            putBool(kPresetFieldRequireParameters, L"require_parameters", L"Require parameter support");
            putBool(kPresetFieldAllowFallbacks, L"allow_fallbacks", L"Allow fallbacks");
            if (!prov.members.empty())
            {
                body.Set(L"provider", std::move(prov));
            }
        }
        const auto putNumber = [&](const wchar_t* field, const wchar_t* jsonKey, const wchar_t* label) {
            const auto text = get(field);
            if (text.empty())
            {
                return;
            }
            if (const auto v = detail::PresetParseDecimal(text))
            {
                body.Set(jsonKey, json::Value::MkNum(*v));
            }
            else
            {
                warnings.push_back(std::wstring{ label } + L": \"" + text + L"\" is not a number (not sent)");
            }
        };
        putNumber(kPresetFieldTemperature, L"temperature", L"Temperature");
        putNumber(kPresetFieldTopP, L"top_p", L"top_p");
        // top_k is an integer in every API that takes it: a fractional or signed value is dropped, not rounded.
        // PresetIsAllDigits caps it at 12 digits, so the double below holds it exactly (< 2^53).
        if (const auto topK = get(kPresetFieldTopK); !topK.empty())
        {
            if (detail::PresetIsAllDigits(topK))
            {
                body.Set(L"top_k", json::Value::MkNum(static_cast<double>(std::wcstoull(topK.c_str(), nullptr, 10))));
            }
            else
            {
                warnings.push_back(L"top_k: \"" + topK + L"\" is not a whole number (not sent)");
            }
        }
        if (const auto extra = get(kPresetFieldExtraBody); !extra.empty())
        {
            const auto parsed = json::Parse(extra);
            if (parsed && parsed->type == json::Value::Type::Obj)
            {
                PresetJsonDeepMerge(body, *parsed);
            }
            else
            {
                warnings.push_back(L"Extra request body: not a JSON object (not sent)");
            }
        }
        return body;
    }

    // Compile a snapshot's effective values into what one Claude launch needs.
    //   a      - the snapshot (provider, routing, preset provenance).
    //   values - EffectiveLaunchValues(a, windowsEnv) for THIS launch.
    //   apiKey - the provider key read for this launch ("" when none was found; ignored without a provider).
    // Returns the compiled launch; `error` is set (and nothing else is meaningful) when a provider launch
    // has no key or no Base URL — the caller must refuse the launch rather than fall back silently, because
    // a fallback would bill another account or run another model.
    inline CompiledClaudeLaunch CompileClaudeLaunch(const LaunchArgs& a, const std::map<std::wstring, std::wstring>& values, std::wstring_view apiKey)
    {
        CompiledClaudeLaunch c;
        c.presetId = a.presetId;
        c.presetName = a.presetName;
        c.providerId = a.providerId;
        c.providerName = a.providerName;
        const auto get = [&](const wchar_t* k) -> std::wstring {
            const auto it = values.find(k);
            return it == values.end() ? std::wstring{} : detail::PresetTrim(it->second);
        };
        const auto putEnv = [&](const std::wstring& name, const std::wstring& value) {
            // Later writers win (the extra-env box overrides a generated key), kept in first-seen position.
            for (auto& kv : c.settingsEnv)
            {
                if (detail::PresetEqualsCi(kv.first, name))
                {
                    kv.second = value;
                    return;
                }
            }
            c.settingsEnv.emplace_back(name, value);
        };
        c.model = get(kPresetFieldModel);

        if (!a.providerId.empty())
        {
            const auto key = detail::PresetTrim(apiKey);
            if (key.empty())
            {
                c.error = L"No API key for " + (a.providerName.empty() ? a.providerId : a.providerName) +
                          L". Type it in Settings \x25B8 Presets, or set " + (a.keyEnv.empty() ? std::wstring{ L"the provider's key variable" } : a.keyEnv) + L".";
                return c;
            }
            const auto baseUrl = get(kPresetFieldBaseUrl);
            if (baseUrl.empty())
            {
                c.error = L"No Base URL for " + (a.providerName.empty() ? a.providerId : a.providerName) + L". Set it in Settings \x25B8 Presets";
                // A built-in with no default URL (RunPod) names the shape of the user's own endpoint, and its
                // variable: the user cannot be expected to know either.
                const auto* def = FindBuiltInLaunchProvider(a.providerId);
                if (def && def->baseUrlExample && *def->baseUrlExample)
                {
                    c.error += L" (" + std::wstring{ def->baseUrlExample } + L")";
                }
                if (const auto it = a.env.find(kPresetFieldBaseUrl); it != a.env.end() && !it->second.empty())
                {
                    c.error += L", or set " + it->second;
                }
                c.error += L".";
                return c;
            }
            putEnv(L"ANTHROPIC_BASE_URL", baseUrl);
            // Empty on purpose: it overrides an Anthropic key the user's environment or settings may carry,
            // and a NON-empty value would open claude's "custom API key" modal (see the header comment).
            putEnv(L"ANTHROPIC_API_KEY", L"");
            if (!c.model.empty())
            {
                const auto name = PresetModelDisplayName(c.model, a.providerName);
                putEnv(L"ANTHROPIC_MODEL", c.model);
                // Every tier: the user's settings may pin an Anthropic id on any of them (the reference
                // machine pins ANTHROPIC_DEFAULT_OPUS_MODEL), which the provider would run — and bill — as is.
                for (const auto* tier : { L"FABLE", L"OPUS", L"SONNET" })
                {
                    putEnv(std::wstring{ L"ANTHROPIC_DEFAULT_" } + tier + L"_MODEL", c.model);
                    putEnv(std::wstring{ L"ANTHROPIC_DEFAULT_" } + tier + L"_MODEL_NAME", name);
                }
                putEnv(L"CLAUDE_CODE_SUBAGENT_MODEL", c.model);
                // Not named `small`: rpcndr.h (pulled in by <windows.h>) #defines small as char.
                const auto background = get(kPresetFieldSmallModel).empty() ? c.model : get(kPresetFieldSmallModel);
                putEnv(L"ANTHROPIC_DEFAULT_HAIKU_MODEL", background);
                putEnv(L"ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME", PresetModelDisplayName(background, a.providerName));
            }
            if (const auto ctx = get(kPresetFieldContext); !ctx.empty())
            {
                if (detail::PresetIsAllDigits(ctx))
                {
                    putEnv(L"CLAUDE_CODE_MAX_CONTEXT_TOKENS", ctx);
                }
                else
                {
                    c.warnings.push_back(L"Context window: \"" + ctx + L"\" is not a whole number (not set)");
                }
            }
            const auto sw = get(kPresetFieldSwitchModel);
            if (!sw.empty() && !detail::PresetEqualsCi(sw, L"none"))
            {
                putEnv(L"ANTHROPIC_CUSTOM_MODEL_OPTION", sw);
                putEnv(L"ANTHROPIC_CUSTOM_MODEL_OPTION_NAME", PresetModelDisplayName(sw, a.providerName));
                putEnv(L"ANTHROPIC_CUSTOM_MODEL_OPTION_DESCRIPTION", sw + L" through " + a.providerName + L", same preset settings");
            }
            const auto body = ComposePresetExtraBody(values, a.routing, c.warnings);
            if (!body.members.empty())
            {
                std::wstring text;
                detail::PresetDumpJson(body, text);
                putEnv(L"CLAUDE_CODE_EXTRA_BODY", text);
            }
            const auto note = get(kPresetFieldIdentityNote);
            if (!note.empty() && !detail::PresetEqualsCi(note, L"none") && !c.model.empty())
            {
                c.appendSystemPrompt = RenderPresetIdentityNote(note, c.model, a.providerName,
                                                                detail::PresetEqualsCi(sw, L"none") ? std::wstring{} : sw);
            }
            c.authTokenEnv = kPresetTokenEnvName;
            c.processEnv.emplace_back(kPresetTokenEnvName, key);
        }

        if (const auto maxOut = get(kPresetFieldMaxOutput); !maxOut.empty())
        {
            if (detail::PresetIsAllDigits(maxOut))
            {
                putEnv(L"CLAUDE_CODE_MAX_OUTPUT_TOKENS", maxOut);
            }
            else
            {
                c.warnings.push_back(L"Max output tokens: \"" + maxOut + L"\" is not a whole number (not set)");
            }
        }
        // Effort rides the settings file as CLAUDE_CODE_EFFORT_LEVEL, not only as --effort: Claude Code obeys that
        // variable over --effort and /effort for the whole session (claude.exe 2.1.291, Nz()), and a user's own
        // ~/.claude/settings.json may set it (the reference machine sets max), which would silently override a
        // --effort-only preset. The --settings file outranks the user's file (HARNESSES.md F1, F9), so the
        // preset's level wins. The flag is passed too, so the command line names the level the observer's
        // `model · effort` adornment shows. "none" = neither: the session then follows Claude Code's own setting
        // (it unsets a template's level). Generated BEFORE the extra env, so a CLAUDE_CODE_EFFORT_LEVEL typed there
        // still wins (the extra-env rule).
        if (const auto effort = get(kPresetFieldEffort); !effort.empty() && !detail::PresetEqualsCi(effort, L"none"))
        {
            if (IsValidPresetEffort(effort))
            {
                std::wstring lower;
                for (const wchar_t ch : effort)
                {
                    lower.push_back(detail::PresetLowerAscii(ch));
                }
                putEnv(L"CLAUDE_CODE_EFFORT_LEVEL", lower);
                c.effort = std::move(lower);
            }
            else
            {
                c.warnings.push_back(L"Effort: \"" + effort + L"\" is not low, medium, high, xhigh, max or none (not set)");
            }
        }
        if (const auto extraEnv = get(kPresetFieldExtraEnv); !extraEnv.empty())
        {
            for (const auto& [name, value] : detail::PresetParseEnvLines(extraEnv, &c.warnings))
            {
                putEnv(name, value);
            }
        }
        // An extra-env line replaced the generated CLAUDE_CODE_EFFORT_LEVEL. Claude Code follows the variable, not
        // --effort, so the Effort box no longer decides the level: say so in the preview and the [preset] log line.
        if (!c.effort.empty())
        {
            for (const auto& [name, value] : c.settingsEnv)
            {
                if (detail::PresetEqualsCi(name, L"CLAUDE_CODE_EFFORT_LEVEL"))
                {
                    if (!detail::PresetEqualsCi(detail::PresetTrim(value), c.effort))
                    {
                        c.warnings.push_back(L"Effort: Extra env vars sets CLAUDE_CODE_EFFORT_LEVEL=" + value +
                                             L", which Claude Code follows instead of the Effort box (" + c.effort + L")");
                    }
                    break;
                }
            }
        }
        if (const auto args = get(kPresetFieldExtraArgs); !args.empty())
        {
            if (const auto bad = PresetForbiddenArgument(args); !bad.empty())
            {
                c.warnings.push_back(L"Extra claude arguments: " + bad + L" is Agentmaster's (all extra arguments dropped)");
            }
            else
            {
                c.extraArgs = args;
            }
        }
        if (!a.presetId.empty())
        {
            c.processEnv.emplace_back(kPresetIdEnvName, a.presetId);
        }
        c.sessionSettings = !c.settingsEnv.empty();
        return c;
    }

    // The launch preview the Presets tab shows (the reference launcher's OPENROUTER_DRY_RUN=1): what the
    // session's settings file sets and what the command line gains. The key never appears — only `maskedKey`.
    //   c         - the compiled launch.
    //   maskedKey - MaskSecret of the key that would be used ("" = none).
    // Returns multi-line text.
    inline std::wstring RenderPresetLaunchPreview(const CompiledClaudeLaunch& c, std::wstring_view maskedKey)
    {
        std::wstring out;
        if (!c.error.empty())
        {
            out += L"Cannot launch: " + c.error + L"\n";
        }
        if (!c.settingsEnv.empty())
        {
            out += L"Settings file env:\n";
            for (const auto& [k, v] : c.settingsEnv)
            {
                out += L"  " + k + L"=" + v + L"\n";
            }
        }
        std::wstring args;
        if (!c.model.empty())
        {
            args += L" --model " + c.model;
        }
        if (!c.effort.empty())
        {
            args += L" --effort " + c.effort;
        }
        if (!c.appendSystemPrompt.empty())
        {
            args += L" --append-system-prompt \"" + c.appendSystemPrompt + L"\"";
        }
        if (!c.extraArgs.empty())
        {
            args += L" " + c.extraArgs;
        }
        if (!args.empty())
        {
            out += L"Command line adds:" + args + L"\n";
        }
        if (!c.authTokenEnv.empty())
        {
            out += L"ANTHROPIC_AUTH_TOKEN=" + (maskedKey.empty() ? std::wstring{ L"(none)" } : std::wstring{ maskedKey }) + L" (only for the claude call)\n";
        }
        for (const auto& w : c.warnings)
        {
            out += L"\x26A0 " + w + L"\n";
        }
        while (!out.empty() && out.back() == L'\n')
        {
            out.pop_back();
        }
        return out;
    }

    // ===== menus and tokens ==================================================================

    // True for a preset pick ("@<id>").
    //   token - a launch target from a menu pick.
    // Never throws.
    inline bool IsPresetToken(std::wstring_view token) noexcept
    {
        return token.size() > 1 && token.front() == kPresetTokenPrefix;
    }

    // The preset id inside a token ("" when `token` is not a preset pick).
    //   token - the launch target.
    inline std::wstring PresetIdFromToken(std::wstring_view token)
    {
        return IsPresetToken(token) ? std::wstring{ token.substr(1) } : std::wstring{};
    }

    // The token for a preset id ("@<id>").
    //   id - the preset id.
    inline std::wstring PresetToken(std::wstring_view id)
    {
        return std::wstring(1, kPresetTokenPrefix) + std::wstring{ id };
    }

    // The model menus' entries: the launch models first, then every VISIBLE preset as {name, "@id"} in list
    // order. AgentFillModelPickItems puts a separator before the first preset entry. With NO preset at all
    // (ShouldOfferAddPresets) the entries are just the models, and the menu shows "Add presets…" in the presets'
    // place instead (the host passes its opener).
    //   models - ParseLaunchModels(AppSettings::launchModels).
    //   cfg    - the presets config.
    // Returns {display name, launch target} pairs.
    inline std::vector<std::pair<std::wstring, std::wstring>> LaunchMenuEntriesFrom(std::vector<std::pair<std::wstring, std::wstring>> models, const LaunchPresetsConfig& cfg)
    {
        for (const auto& p : cfg.presets)
        {
            if (!p.hidden && !p.id.empty())
            {
                models.emplace_back(PresetDisplayName(p), PresetToken(p.id));
            }
        }
        return models;
    }

    // The launch-models SPEC text extended with the visible presets ("Name | @id" lines), for consumers
    // that parse a spec (the /handover per-message hint, ParseHandoverArgsHints). A preset name's ';' and
    // '|' become spaces, because those characters separate entries and sides in a spec.
    //   launchModelsSpec - AppSettings::launchModels.
    //   cfg              - the presets config.
    // Returns the extended spec.
    inline std::wstring LaunchMenuSpecFrom(std::wstring_view launchModelsSpec, const LaunchPresetsConfig& cfg)
    {
        std::wstring out{ launchModelsSpec };
        for (const auto& p : cfg.presets)
        {
            if (p.hidden || p.id.empty())
            {
                continue;
            }
            std::wstring name = PresetDisplayName(p);
            for (auto& ch : name)
            {
                if (ch == L';' || ch == L'|' || ch == L'\n' || ch == L'\r')
                {
                    ch = L' ';
                }
            }
            out += L"\n" + name + L" | " + PresetToken(p.id);
        }
        return out;
    }

    // True while the list holds NO preset at all: the initial state (the user's rule, 2026-10-06 — no template is
    // added by itself). Every launch menu then shows "Add presets…" where the presets would be, and its dialog says
    // where the list is (AddPresetsHelpText). A list whose presets are ALL hidden offers nothing: the user has
    // presets and chose to keep them out of the menus.
    //   cfg - the presets config.
    // Never throws.
    inline bool ShouldOfferAddPresets(const LaunchPresetsConfig& cfg) noexcept
    {
        return cfg.presets.empty();
    }

    // The "Add presets…" menu item (AgentFillModelPickItems): its label and its tooltip.
    inline constexpr const wchar_t* kAddPresetsMenuText = L"Add presets\x2026";
    inline constexpr const wchar_t* kAddPresetsMenuTip = L"You have no presets yet. Shows where to add one: Settings (\x2699) \x2192 Presets \x2192 TEMPLATES.";
    // The title of the dialog the item opens (each host shows it in its own dialog layer).
    inline constexpr const wchar_t* kAddPresetsHelpTitle = L"Add presets";

    // The body of the "Add presets" dialog: what a preset is, the numbered steps to add one (ending with Save, because
    // the Settings card applies nothing before it), and the key an OpenRouter preset needs.
    //   inAgentManager - true when the dialog opens over the Agent Manager page, whose toolbar holds the Settings
    //                    button: the "open the Agent Manager" step is then left out and the steps renumber.
    // Returns the text ('\n'-separated lines).
    inline std::wstring AddPresetsHelpText(bool inAgentManager)
    {
        std::vector<std::wstring> steps;
        if (!inAgentManager)
        {
            steps.emplace_back(L"Open the Agent Manager: the Home button at the left end of the tab strip.");
        }
        steps.emplace_back(L"Click \x2699 (Settings) in its toolbar, then the Presets tab.");
        // Two literals: a hex escape would swallow the "Add" that follows it.
        steps.emplace_back(L"Under TEMPLATES, click Add next to a tested configuration, or \x201C" L"Add all tested templates\x201D.");
        steps.emplace_back(L"Click Save.");
        std::wstring out = L"You have no presets yet. A preset starts a Claude session with another provider or model setup, for example Kimi K3, Qwen3.8 or GLM-5.3 through OpenRouter.\n\nTo add one:\n";
        for (size_t i = 0; i < steps.size(); ++i)
        {
            out += std::to_wstring(i + 1) + L". " + steps[i] + L"\n";
        }
        out += L"\nYour presets are then listed at the bottom of this menu. An OpenRouter preset also needs your OpenRouter API key: type it in the OpenRouter block under PROVIDERS on the same tab, or set OPENROUTER_API_KEY.";
        return out;
    }

    // ===== launch-presets.json ===============================================================

    // Serialize the config (compact JSON). Empty sparse maps and default flags are left out. The "seededTemplates"
    // list of release 0.6.8.18 is never written: a file without it is never pruned (DropUntouchedAutoAddedPresets).
    //   cfg - the config.
    // Returns the file text.
    inline std::wstring LaunchPresetsConfigToJson(const LaunchPresetsConfig& cfg)
    {
        json::Value root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        json::Value providers = json::Value::MkArr();
        for (const auto& e : cfg.providers)
        {
            json::Value o = json::Value::MkObj();
            o.Set(L"id", json::Value::MkStr(e.id));
            if (e.custom)
            {
                o.Set(L"custom", json::Value::MkBool(true));
                o.Set(L"name", json::Value::MkStr(e.name));
                o.Set(L"envPrefix", json::Value::MkStr(e.envPrefix));
            }
            if (!e.anthropicBaseUrl.empty())
            {
                o.Set(L"anthropicBaseUrl", json::Value::MkStr(e.anthropicBaseUrl));
            }
            if (!e.openaiBaseUrl.empty())
            {
                o.Set(L"openaiBaseUrl", json::Value::MkStr(e.openaiBaseUrl));
            }
            providers.Push(std::move(o));
        }
        root.Set(L"providers", std::move(providers));
        json::Value presets = json::Value::MkArr();
        for (const auto& p : cfg.presets)
        {
            json::Value o = json::Value::MkObj();
            o.Set(L"id", json::Value::MkStr(p.id));
            if (!p.templateId.empty())
            {
                o.Set(L"templateId", json::Value::MkStr(p.templateId));
            }
            if (p.hidden)
            {
                o.Set(L"hidden", json::Value::MkBool(true));
            }
            json::Value fields = json::Value::MkObj();
            for (const auto& [k, v] : p.fields)
            {
                if (!v.empty())
                {
                    fields.Set(k, json::Value::MkStr(v));
                }
            }
            if (!fields.members.empty())
            {
                o.Set(L"fields", std::move(fields));
            }
            presets.Push(std::move(o));
        }
        root.Set(L"presets", std::move(presets));
        return json::Dump(root);
    }

    // Parse the config. Tolerant by design: a missing or unreadable file reads as an EMPTY config (no preset: the
    // menus then offer "Add presets…"), a malformed entry is skipped, and duplicate ids keep the first. A file
    // written by release 0.6.8.18 also loses the template presets that build added by itself and the user never
    // changed (DropUntouchedAutoAddedPresets, run on its "seededTemplates" list).
    //   text            - the file text.
    //   droppedAutoAdded - when not null, receives the ids of the presets that cleanup left out (the OS loader logs
    //                      them once per file stamp); cleared first.
    // Returns the config (never throws on bad input).
    inline LaunchPresetsConfig LaunchPresetsConfigFromJson(std::wstring_view text, std::vector<std::wstring>* droppedAutoAdded = nullptr)
    {
        if (droppedAutoAdded)
        {
            droppedAutoAdded->clear();
        }
        LaunchPresetsConfig cfg;
        const auto parsed = json::Parse(text);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return cfg;
        }
        // Release 0.6.8.18's record of the templates it added by itself. Read only for the cleanup below: this build
        // adds nothing by itself, and its Save no longer writes the list.
        std::vector<std::wstring> autoAddedIds;
        if (const auto* seeded = parsed->Find(L"seededTemplates"); seeded && seeded->type == json::Value::Type::Arr)
        {
            for (const auto& v : seeded->arr)
            {
                if (v.type == json::Value::Type::Str && !v.str.empty())
                {
                    autoAddedIds.push_back(v.str);
                }
            }
        }
        if (const auto* providers = parsed->Find(L"providers"); providers && providers->type == json::Value::Type::Arr)
        {
            for (const auto& v : providers->arr)
            {
                if (v.type != json::Value::Type::Obj || v.StrAt(L"id").empty())
                {
                    continue;
                }
                LaunchProviderEntry e;
                e.id = v.StrAt(L"id");
                e.custom = v.BoolAt(L"custom");
                e.name = v.StrAt(L"name");
                e.envPrefix = SanitizeLaunchEnvPrefix(v.StrAt(L"envPrefix"));
                e.anthropicBaseUrl = v.StrAt(L"anthropicBaseUrl");
                e.openaiBaseUrl = v.StrAt(L"openaiBaseUrl");
                const bool dup = std::any_of(cfg.providers.begin(), cfg.providers.end(), [&](const auto& x) { return x.id == e.id; });
                if (!dup)
                {
                    cfg.providers.push_back(std::move(e));
                }
            }
        }
        if (const auto* presets = parsed->Find(L"presets"); presets && presets->type == json::Value::Type::Arr)
        {
            for (const auto& v : presets->arr)
            {
                if (v.type != json::Value::Type::Obj || v.StrAt(L"id").empty())
                {
                    continue;
                }
                LaunchPreset p;
                p.id = v.StrAt(L"id");
                p.templateId = v.StrAt(L"templateId");
                p.hidden = v.BoolAt(L"hidden");
                if (const auto* fields = v.Find(L"fields"); fields && fields->type == json::Value::Type::Obj)
                {
                    for (const auto& [k, fv] : fields->members)
                    {
                        if (fv.type == json::Value::Type::Str && !fv.str.empty())
                        {
                            p.fields[k] = fv.str;
                        }
                    }
                }
                if (!FindLaunchPreset(cfg, p.id))
                {
                    cfg.presets.push_back(std::move(p));
                }
            }
        }
        auto dropped = DropUntouchedAutoAddedPresets(cfg, autoAddedIds);
        if (droppedAutoAdded)
        {
            *droppedAutoAdded = std::move(dropped);
        }
        return cfg;
    }
}
