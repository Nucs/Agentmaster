// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — PRESETS, the OS half (doc/agentmaster/HARNESSES.md). LaunchPresets.h is the pure core;
// this header adds what touches the machine:
//   * the two env lookups the value tiers read — Agentmaster's env list (Settings: Global + Per-
//     directory) and the Windows environment (the process env, then the registry for a name set after
//     Agentmaster started);
//   * provider keys in Windows Credential Manager (a typed key never goes into a profile file);
//   * the launch-presets.json loader, cached by file stamp (a read never writes the file, and adds nothing: no
//     file means no preset, and the menus then offer "Add presets…" — ShouldOfferAddPresets);
//   * the session's snapshot in the session store (`launchArgs`), and the launch-time compile that reads
//     the key again.
//
// Header-only (the PortableInstall.h idiom) and included only by TerminalApp TUs (the launch seams, the
// menus, the Settings cog), so the engine units the CLI and the test harness link stay free of it.
// Credential Manager lives in advapi32 — linked by the #pragma below.
// Exceptions: every function here is noexcept in effect except for std::bad_alloc; Win32 failures read as
// "absent" (an empty key, an unset variable, an empty config) and are logged where a user would
// otherwise see a silent no-op.

#pragma once

#include <windows.h>
#include <wincred.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ClaudeSpawn.h" // ResolveSessionEnv, NewSessionId, AppendStateLog, AgentmasterStateDir
#include "LaunchPresets.h"
#include "Persistence.h" // LaunchPresetsPath / LoadLaunchPresetsText / SaveLaunchPresetsText
#include "SessionStore.h" // GetSessionStoreField / SetSessionStoreField (the launchArgs key)

#pragma comment(lib, "advapi32.lib") // CredReadW / CredWriteW / CredDeleteW / RegGetValueW

namespace Agentmaster
{
    // ===== env lookups =======================================================================

    // One variable from the Windows environment, as a NEW managed session would see it. The process env
    // comes first: managed sessions inherit Agentmaster's own environment block (ENV_VARS.md §1), which
    // is a snapshot from app start. A name the process does not have is then read from the registry
    // (HKCU\Environment, then the machine's Session Manager\Environment, REG_EXPAND_SZ expanded), so a key
    // the user sets AFTER starting Agentmaster works without a restart. The launch seam puts the value it
    // finds into the session's env itself, so the two can never disagree.
    //   name - the variable name.
    // Returns the value, or "" when it is set nowhere. Never throws except std::bad_alloc.
    inline std::wstring ReadWindowsEnvVar(std::wstring_view name)
    {
        if (name.empty())
        {
            return {};
        }
        const std::wstring n{ name };
        const DWORD needed = ::GetEnvironmentVariableW(n.c_str(), nullptr, 0);
        if (needed > 1)
        {
            std::wstring value(needed, L'\0');
            const DWORD got = ::GetEnvironmentVariableW(n.c_str(), value.data(), needed);
            if (got > 0 && got < needed)
            {
                value.resize(got);
                return value;
            }
        }
        // RRF_RT_REG_SZ alone: without RRF_NOEXPAND, RegGetValueW expands a REG_EXPAND_SZ value and reports
        // it as REG_SZ (combining RRF_RT_REG_EXPAND_SZ without RRF_NOEXPAND is an invalid-parameter error).
        const auto readKey = [&](HKEY root, const wchar_t* subKey) -> std::wstring {
            DWORD bytes = 0;
            if (::RegGetValueW(root, subKey, n.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
            {
                return {};
            }
            std::wstring value(bytes / sizeof(wchar_t), L'\0');
            if (::RegGetValueW(root, subKey, n.c_str(), RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
            {
                return {};
            }
            value.resize(bytes / sizeof(wchar_t));
            while (!value.empty() && value.back() == L'\0')
            {
                value.pop_back(); // the stored terminator
            }
            return value;
        };
        if (auto v = readKey(HKEY_CURRENT_USER, L"Environment"); !v.empty())
        {
            return v;
        }
        return readKey(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    }

    // The Windows-environment lookup the value tiers take (ReadWindowsEnvVar per call).
    // Returns the lookup.
    inline PresetEnvLookup WindowsEnvLookup()
    {
        return [](std::wstring_view name) { return ReadWindowsEnvVar(name); };
    }

    // Agentmaster's env list as a lookup: the Settings cog's Global variables, with the launch
    // directory's Per-directory variables over them when `dir` is given (ResolveSessionEnv — the very
    // merge every managed spawn applies). Names compare case-insensitively, like Windows.
    //   settings - the app settings (Global env).
    //   dir      - the launch directory ("" = Global only: what a Settings placeholder shows).
    // Returns the lookup (it captures a copy, so it stays valid after `settings` changes).
    inline PresetEnvLookup AgentmasterEnvLookup(const AppSettings& settings, std::wstring_view dir)
    {
        auto vars = dir.empty() ? MergeSessionEnv(settings.env, L"") : ResolveSessionEnv(settings, dir);
        return [vars = std::move(vars)](std::wstring_view name) -> std::wstring {
            std::wstring found;
            for (const auto& [k, v] : vars)
            {
                if (detail::PresetEqualsCi(k, name))
                {
                    found = v; // last assignment wins, the merge's own rule
                }
            }
            return found;
        };
    }

    // ===== provider keys (Windows Credential Manager) =========================================

    // The Credential Manager target name of a provider's key. Not per profile on purpose: the dev and
    // release installs (two profiles) then share one typed key, the way they share the user's env vars.
    //   providerId - the provider id.
    // Returns e.g. "Agentmaster:provider:openrouter".
    inline std::wstring PresetCredentialTarget(std::wstring_view providerId)
    {
        return L"Agentmaster:provider:" + std::wstring{ providerId };
    }

    // The key stored for a provider.
    //   providerId - the provider id ("" = none).
    // Returns the key, or "" when none is stored or Credential Manager refused the read.
    inline std::wstring ReadPresetProviderKey(std::wstring_view providerId)
    {
        if (providerId.empty())
        {
            return {};
        }
        const auto target = PresetCredentialTarget(providerId);
        PCREDENTIALW cred = nullptr;
        if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred)
        {
            return {}; // ERROR_NOT_FOUND is the normal "no key typed" case — not worth a log line
        }
        std::wstring key;
        if (cred->CredentialBlob && cred->CredentialBlobSize >= sizeof(wchar_t))
        {
            key.assign(reinterpret_cast<const wchar_t*>(cred->CredentialBlob), cred->CredentialBlobSize / sizeof(wchar_t));
        }
        ::CredFree(cred);
        return detail::PresetTrim(key);
    }

    // True when Credential Manager holds a key for the provider (the Settings tab's "stored" line).
    //   providerId - the provider id.
    inline bool HasPresetProviderKey(std::wstring_view providerId)
    {
        return !ReadPresetProviderKey(providerId).empty();
    }

    // Store a provider key (replacing any older one). The key is the credential's blob; Credential
    // Manager protects it with the user's Windows login.
    //   providerId - the provider id.
    //   key        - the key (trimmed; empty = refused — use DeletePresetProviderKey to remove one).
    // Returns true when the key is stored. A failure is logged to hooks.log (without the key).
    inline bool WritePresetProviderKey(std::wstring_view providerId, std::wstring_view key)
    {
        auto value = detail::PresetTrim(key);
        if (providerId.empty() || value.empty() || value.size() * sizeof(wchar_t) > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        {
            return false;
        }
        auto target = PresetCredentialTarget(providerId);
        std::wstring user{ L"Agentmaster" };
        std::wstring comment{ L"Agentmaster preset provider key (Settings > Presets)" };
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = target.data();
        cred.Comment = comment.data();
        cred.UserName = user.data();
        cred.CredentialBlobSize = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(value.data());
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE; // survives logoff; per Windows user, never roams
        const bool ok = ::CredWriteW(&cred, 0) != FALSE;
        const DWORD err = ok ? 0 : ::GetLastError();
        ::SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); // drop our plain copy now
        if (!ok)
        {
            AppendStateLog(L"hooks.log", L"[preset] could not store the " + std::wstring{ providerId } + L" key in Credential Manager (error " + std::to_wstring(err) + L")\n");
        }
        return ok;
    }

    // Remove a provider's stored key (the Settings tab's "Remove stored key").
    //   providerId - the provider id.
    // Returns true when no key is stored afterwards (including "there was none").
    inline bool DeletePresetProviderKey(std::wstring_view providerId)
    {
        if (providerId.empty())
        {
            return true;
        }
        const auto target = PresetCredentialTarget(providerId);
        if (::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
        {
            return true;
        }
        return ::GetLastError() == ERROR_NOT_FOUND;
    }

    // ===== launch-presets.json ================================================================

    namespace detail
    {
        // The process-wide cache of the parsed config, keyed by the file's existence, size
        // and last-write time. Every menu open asks for the config; parsing on each would re-read the file
        // for a list that changes only on a Settings Save.
        struct LaunchPresetsCache
        {
            std::mutex lock;
            bool loaded{ false };
            bool exists{ false };
            uint64_t size{ 0 };
            FILETIME mtime{};
            LaunchPresetsConfig cfg;
        };

        // The one cache (leaked on purpose: never destroyed at process exit, so no window's teardown can
        // touch a destroyed mutex). Never throws except std::bad_alloc on first use.
        inline LaunchPresetsCache& PresetsCache()
        {
            static auto* cache = new LaunchPresetsCache();
            return *cache;
        }
    }

    // The preset config, exactly as the file holds it: no template is added by itself (the user's rule,
    // 2026-10-06), so a missing or unreadable file reads as an EMPTY list and every launch menu offers "Add
    // presets…". A file written by release 0.6.8.18 loses, in memory, the template presets that build added by
    // itself and the user never changed (DropUntouchedAutoAddedPresets; logged once per file stamp as a [preset]
    // line); the next Settings Save writes the file without them. Cached by file stamp.
    // Returns a copy (callers may edit it freely; SaveLaunchPresetsConfig persists an edited copy).
    inline LaunchPresetsConfig LoadLaunchPresetsConfig()
    {
        const auto path = LaunchPresetsPath();
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        const bool exists = ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) != FALSE;
        const uint64_t size = exists ? ((static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow) : 0;
        const FILETIME mtime = exists ? fad.ftLastWriteTime : FILETIME{};
        auto& cache = detail::PresetsCache();
        std::lock_guard<std::mutex> guard(cache.lock);
        if (cache.loaded && cache.exists == exists && cache.size == size && ::CompareFileTime(&cache.mtime, &mtime) == 0)
        {
            return cache.cfg;
        }
        std::vector<std::wstring> dropped;
        LaunchPresetsConfig cfg = exists ? LaunchPresetsConfigFromJson(LoadLaunchPresetsText(), &dropped) : LaunchPresetsConfig{};
        if (!dropped.empty())
        {
            // Once per parse (the cache keeps the result until the file changes), so a menu open never repeats it.
            std::wstring ids;
            for (const auto& id : dropped)
            {
                ids += (ids.empty() ? L"" : L", ") + id;
            }
            AppendStateLog(L"hooks.log", L"[preset] launch-presets.json: left out " + std::to_wstring(dropped.size()) +
                                             L" template preset(s) release 0.6.8.18 added by itself and nobody changed (" + ids +
                                             L"); presets start empty now, and the next Settings Save removes them from the file\n");
        }
        cache.loaded = true;
        cache.exists = exists;
        cache.size = size;
        cache.mtime = mtime;
        cache.cfg = cfg;
        return cfg;
    }

    // Persist the config (the Settings cog's Save) and drop the cache, so every window's next menu reads it.
    //   cfg - the whole config.
    // Returns true when the file was written.
    inline bool SaveLaunchPresetsConfig(const LaunchPresetsConfig& cfg)
    {
        const bool ok = SaveLaunchPresetsText(LaunchPresetsConfigToJson(cfg));
        auto& cache = detail::PresetsCache();
        std::lock_guard<std::mutex> guard(cache.lock);
        cache.loaded = false; // even a failed write may have replaced the file: re-read it next time
        return ok;
    }

    // 64 bits of entropy for a new preset or custom-provider id (a fresh session-id GUID, hashed).
    inline uint64_t NewPresetEntropy()
    {
        return detail::PresetFnv1a(NewSessionId());
    }

    // ===== menus =============================================================================

    // Every model menu's entries: the launch models (ParseLaunchModels), then the visible presets as
    // "@<id>" picks. The one function the "Open New Session Here" / "Fork session" builders, the tab menu
    // and the Launch button's hover menu call, so they all list the same presets in the same order.
    //   launchModelsSpec - AppSettings::launchModels.
    // Returns {display name, launch target} pairs.
    inline std::vector<std::pair<std::wstring, std::wstring>> LaunchMenuEntries(std::wstring_view launchModelsSpec)
    {
        return LaunchMenuEntriesFrom(ParseLaunchModels(launchModelsSpec), LoadLaunchPresetsConfig());
    }

    // The launch-models spec with the visible presets appended, for spec parsers (the /handover
    // per-message hint: "/handover [mimo] ..." picks a preset by its name).
    //   launchModelsSpec - AppSettings::launchModels.
    // Returns the extended spec.
    inline std::wstring LaunchMenuSpec(std::wstring_view launchModelsSpec)
    {
        return LaunchMenuSpecFrom(launchModelsSpec, LoadLaunchPresetsConfig());
    }

    // True while the user has no preset at all (ShouldOfferAddPresets over the cached config): the ONE question every
    // host asks before it hands a menu its "Add presets…" opener (AgentManagerContent / TerminalPage
    // _AddPresetsOpener, the Launch button's hover menu), so the menus can never disagree about it.
    inline bool ShouldOfferAddPresets()
    {
        return ShouldOfferAddPresets(LoadLaunchPresetsConfig());
    }

    // ===== the session snapshot ===============================================================

    // The snapshot for a NEW launch target picked in a menu.
    //   token    - "@<presetId>" or a plain model id ("" = no pick: nothing to snapshot).
    //   settings - the app settings (Agentmaster's env list for tier 2).
    //   dir      - the launch directory (its Per-directory env joins tier 2).
    //   error    - receives a user-facing reason when a preset pick cannot be honored (the preset was
    //              deleted, or its provider was - PresetLaunchRefusal).
    // Returns the snapshot, or nullopt for "" or a failed preset pick (then `error` is set).
    inline std::optional<LaunchArgs> LaunchArgsForTarget(std::wstring_view token, const AppSettings& settings, std::wstring_view dir, std::wstring& error)
    {
        error.clear();
        if (detail::PresetTrim(token).empty())
        {
            return std::nullopt;
        }
        if (!IsPresetToken(token))
        {
            return SnapshotModelLaunch(token);
        }
        const auto cfg = LoadLaunchPresetsConfig();
        const auto id = PresetIdFromToken(token);
        const auto* preset = FindLaunchPreset(cfg, id);
        if (!preset)
        {
            error = L"The preset \"" + id + L"\" no longer exists. Pick another one, or recreate it in Settings \x25B8 Presets.";
            return std::nullopt;
        }
        // A removed custom provider: refuse, never fall back to the Claude login (Settings promises this
        // on the provider's remove button).
        if (auto refusal = PresetLaunchRefusal(*preset, cfg); !refusal.empty())
        {
            error = std::move(refusal);
            return std::nullopt;
        }
        return SnapshotPresetLaunch(*preset, cfg, AgentmasterEnvLookup(settings, dir));
    }

    // A session's stored snapshot (session-store/<sid>.json, key "launchArgs").
    //   sessionId - the session id.
    // Returns the snapshot, or nullopt when the session has none (it launched on Default) or it cannot be
    // read by this build.
    inline std::optional<LaunchArgs> LoadStoredLaunchArgs(std::wstring_view sessionId)
    {
        if (sessionId.empty())
        {
            return std::nullopt;
        }
        return LaunchArgsFromJson(GetSessionStoreField(std::wstring{ sessionId }, kSessionStoreLaunchArgsKey));
    }

    // Store a session's snapshot. Deduplicated by the store (an identical value writes nothing), so every
    // relaunch can call it without disk churn.
    //   sessionId - the session id.
    //   args      - the snapshot.
    inline void StoreLaunchArgs(std::wstring_view sessionId, const LaunchArgs& args)
    {
        if (sessionId.empty())
        {
            return;
        }
        if (!SetSessionStoreField(std::wstring{ sessionId }, kSessionStoreLaunchArgsKey, LaunchArgsToJson(args)))
        {
            AppendStateLog(L"hooks.log", L"[preset] " + ShortId(std::wstring{ sessionId }) + L" could not store launchArgs (the next resume would start on Default)\n");
        }
    }

    // The key one launch uses: Credential Manager, then Agentmaster's env list (Global + the launch
    // directory's), then the Windows environment. Read at EVERY launch and never stored in a snapshot.
    //   a        - the snapshot (its key references).
    //   settings - the app settings.
    //   dir      - the launch directory.
    // Returns the key ("" when none is found or the snapshot has no provider).
    inline std::wstring ResolveLaunchKey(const LaunchArgs& a, const AppSettings& settings, std::wstring_view dir)
    {
        if (a.providerId.empty())
        {
            return {};
        }
        if (auto k = ReadPresetProviderKey(a.keyCredential); !k.empty())
        {
            return k;
        }
        if (a.keyEnv.empty())
        {
            return {};
        }
        if (auto k = detail::PresetTrim(AgentmasterEnvLookup(settings, dir)(a.keyEnv)); !k.empty())
        {
            return k;
        }
        return detail::PresetTrim(ReadWindowsEnvVar(a.keyEnv));
    }

    // Compile a snapshot for one launch: the Windows-tier values and the key are read NOW.
    //   a        - the snapshot.
    //   settings - the app settings.
    //   dir      - the launch directory.
    //   out      - receives the compiled launch.
    //   error    - receives the user-facing reason when the launch must be refused (no key / no Base URL).
    // Returns true when the launch may proceed. Every dropped value is logged once, as a [preset] line.
    inline bool CompileLaunchForSpawn(const LaunchArgs& a, const AppSettings& settings, std::wstring_view dir, CompiledClaudeLaunch& out, std::wstring& error)
    {
        error.clear();
        out = CompileClaudeLaunch(a, EffectiveLaunchValues(a, WindowsEnvLookup()), ResolveLaunchKey(a, settings, dir));
        for (const auto& w : out.warnings)
        {
            AppendStateLog(L"hooks.log", L"[preset] " + (a.presetId.empty() ? std::wstring{ L"(model pick)" } : a.presetId) + L": " + w + L"\n");
        }
        if (!out.error.empty())
        {
            error = out.error;
            return false;
        }
        return true;
    }
}
