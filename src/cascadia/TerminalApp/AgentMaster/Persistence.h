// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — persistence (DESIGN §13) + plan templates / apply-to-many (DESIGN §10).
// Sessions (queue + autorunner + metadata) and plan templates serialize to JSON under the
// app state dir. Restore never replays already-Sent prompts (statuses are preserved).
//
// Enum<->string and the struct<->JSON mappings are pure (Json.h only) and unit-tested.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map> // DirColorIndex (the batch color lookup)
#include <unordered_set>
#include <utility>
#include <vector>

#include "Json.h"
#include "SessionModels.h"

namespace Agentmaster
{
    // ---- enum <-> string ----
    std::wstring ToString(SessionState s);
    SessionState SessionStateFromString(std::wstring_view s);
    // The PERSISTED word of a Tests Autorunner mode — what sessions.json, settings.json
    // (defaultAutorunnerMode) and the `agentmaster` CLI's JSON carry. Never the UI wording
    // (AutorunnerModeDisplayName): renaming these would orphan every stored value.
    //   m       — the mode.
    //   returns — L"Full" or L"Off".
    //   throws  — std::bad_alloc on allocation failure only.
    std::wstring ToString(AutorunnerMode m);
    // Parse a persisted mode word. TOTAL: only "Full" reads as Full; "Off", an unknown word and the
    // retired "SemiAuto" (removed 2026-10-09) all read as Off, so a value written by an older build can
    // never make a session start sending by itself.
    //   s       — the stored word (case-sensitive).
    //   returns — AutorunnerMode::Full or AutorunnerMode::Off.
    AutorunnerMode AutorunnerModeFromString(std::wstring_view s);
    std::wstring ToString(PromptStatus s);
    PromptStatus PromptStatusFromString(std::wstring_view s);
    std::wstring ToString(PromptGate g);
    PromptGate PromptGateFromString(std::wstring_view s);
    std::wstring ToString(PromptOrigin o);
    PromptOrigin PromptOriginFromString(std::wstring_view s);
    std::wstring ToString(ExplorerSort s);
    ExplorerSort ExplorerSortFromString(std::wstring_view s);
    std::wstring ToString(TabRenameCommitMode m);
    TabRenameCommitMode TabRenameCommitModeFromString(std::wstring_view s);
    std::wstring ToString(FavoriteIcon i);
    FavoriteIcon FavoriteIconFromString(std::wstring_view s);
    std::wstring ToString(TabColorMode m);
    TabColorMode TabColorModeFromString(std::wstring_view s);
    std::wstring ToString(TabTitleNaming n);
    TabTitleNaming TabTitleNamingFromString(std::wstring_view s);
    std::wstring ToString(TabTitleCase c);
    TabTitleCase TabTitleCaseFromString(std::wstring_view s);
    // Agentmaster (tab sizing): the "tabWidthMode" / "tabTitleFontWeight" settings tokens — "terminal" /
    // "equal" / "titleLength" / "compact" (Windows Terminal's own tabWidthMode words, plus "terminal")
    // and "normal" / "semiBold" / "bold". An unknown token parses to the default (Terminal / Normal).
    std::wstring ToString(TabStripWidthMode m);
    TabStripWidthMode TabStripWidthModeFromString(std::wstring_view s);
    std::wstring ToString(TabTitleFontWeight w);
    TabTitleFontWeight TabTitleFontWeightFromString(std::wstring_view s);

    // ---- struct <-> json::Value ----
    json::Value ToJson(const QueuedPrompt& p);
    QueuedPrompt PromptFromJson(const json::Value& v);
    json::Value ToJson(const AutorunnerState& a);
    AutorunnerState AutorunnerFromJson(const json::Value& v);
    json::Value ToJson(const SessionInfo& s);
    SessionInfo SessionFromJson(const json::Value& v);
    json::Value ToJson(const PlanTemplate& t);
    PlanTemplate TemplateFromJson(const json::Value& v);
    json::Value ToJson(const AppSettings& s);
    AppSettings AppSettingsFromJson(const json::Value& v);
    // Workspace persistence (M10): the per-window record + its parts.
    json::Value ToJson(const ManagerLayout& l);
    ManagerLayout ManagerLayoutFromJson(const json::Value& v);
    json::Value ToJson(const WindowGeometry& g);
    WindowGeometry GeometryFromJson(const json::Value& v);
    json::Value ToJson(const TabEntry& t);
    TabEntry TabEntryFromJson(const json::Value& v);
    json::Value ToJson(const ManagerState& m);
    ManagerState ManagerStateFromJson(const json::Value& v);
    json::Value ToJson(const WindowRecord& w);
    WindowRecord WindowRecordFromJson(const json::Value& v);

    // ---- whole-document (de)serialization (pure; testable) ----
    std::wstring SerializeSessions(const std::vector<SessionInfo>& sessions);
    std::vector<SessionInfo> DeserializeSessions(std::wstring_view text);
    std::wstring SerializeTemplates(const std::vector<PlanTemplate>& templates);
    std::vector<PlanTemplate> DeserializeTemplates(std::wstring_view text);
    // Recent working directories (MRU) for the Launch path-picker. Front == most recent.
    std::wstring SerializeRecentDirs(const std::vector<std::wstring>& dirs);
    std::vector<std::wstring> DeserializeRecentDirs(std::wstring_view text);
    // Agentmaster (launch-model picker -> "Specify..."): the MODEL LIST — the durable set of model
    // ids the Specify prompt's drop-down offers, so a model you looked up once is one click away
    // forever after. It accumulates two ways and is reset one way:
    //   * SPECIFYING a model (typing one into the prompt and committing) ADDS it, newest first;
    //   * FETCHING replaces the whole list with the freshly downloaded catalogs (ResetModelList) —
    //     the deliberate "rebuild this from the published lists" action, so a list that has grown
    //     stale or cluttered has an obvious way back.
    // The CONFIGURED launch models (AppSettings.launchModels) are NOT stored here: they are merged
    // in front at display time (MergeModelIdGroups), so they always appear and always track the
    // settings — storing a copy would go stale the moment the cog is edited.
    //
    // Its own file (model-list.json), NOT an AppSettings field: it is written from any window on
    // any pick, and settings.json is a read-modify-write document several surfaces already contend
    // for (the updater's RMW, the cog's Save) — an append must never risk clobbering it.
    //
    // The same file also holds the LAST SPECIFIED MODEL (its "last" key): the id the user last
    // COMMITTED in the prompt, which every prompt then opens pre-filled with — in every window, and
    // after a restart. It is its own value, not "the list's first entry", because a Fetch rebuilds the
    // list: right after one, the front is a catalog id the user never typed. So a Fetch KEEPS it, and
    // only the next commit replaces it. Same file, so one commit updates both in a single atomic
    // write and the two can never disagree. One profile = one value: the dev and release installs
    // keep their own.
    inline constexpr size_t kMaxModelListEntries = 100; // a fetched catalog is tens of ids; this bounds a runaway file, not normal use
    // The model-list.json document: {"version":1,"models":[...],"last":"<id>"}. The "last" key is
    // left out when `lastSpecified` is empty, so a file with no remembered model reads exactly as it
    // did before the key existed (and an older build that rewrites the file just drops it).
    //   models        - the saved ids, in drop-down order; written exactly as given (callers trim
    //                   and dedupe through PushModelListEntry / ResetModelList).
    //   lastSpecified - the id the prompt opens with; "" leaves the key out.
    //   returns       - the JSON text.
    // Throws std::bad_alloc only.
    std::wstring SerializeModelList(const std::vector<std::wstring>& models, std::wstring_view lastSpecified = {});
    // The saved ids out of model-list.json text, in stored order. Never throws on content: a missing,
    // corrupt or wrong-shaped document reads as no ids (the prompt then offers only the configured
    // Launch models), and blank / non-string entries are skipped.
    //   text    - the file's text ("" for a missing file).
    //   returns - the ids.
    // Throws std::bad_alloc only.
    std::vector<std::wstring> DeserializeModelList(std::wstring_view text);
    // The LAST SPECIFIED MODEL out of model-list.json text (its "last" key) — what the prompt's box
    // opens with. Trimmed, so a hand-edited "  " reads as none rather than as a blank pre-fill.
    //   text    - the file's text ("" for a missing file).
    //   returns - the id, or "" when the key is absent, not a string, blank, or the text is not JSON.
    // Throws std::bad_alloc only.
    std::wstring DeserializeLastSpecifiedModel(std::wstring_view text);
    // Push `id` to the FRONT of the list (pure; the caller persists the result). Trims; an empty /
    // whitespace-only id is a no-op; an id already present is MOVED to the front rather than
    // duplicated (matched case-INsensitively — "Opus" and "opus" are one model — while the NEW
    // spelling is what gets stored, so the list shows what you last typed); capped at `cap`
    // (oldest dropped). PURE + unit-tested.
    std::vector<std::wstring> PushModelListEntry(std::vector<std::wstring> models, std::wstring_view id, size_t cap = kMaxModelListEntries);
    // Open-at-exit window manifest (M10 Increment 3 refinement; PERSISTENCE.md §13.5): the set of
    // windowIds that were OPEN when the app last exited — distinct from "every record ever," so the
    // startup auto-reopen offers exactly the last-open windows (a window closed mid-session is pruned
    // from the set and not re-offered). A flat JSON array of windowId strings (it's a set; order is
    // irrelevant). The Engine is the sole writer (it holds the live id set); the WindowEmperor reads
    // it at startup to decide which records to reopen.
    std::wstring SerializeOpenWindows(const std::vector<std::wstring>& windowIds);
    std::vector<std::wstring> DeserializeOpenWindows(std::wstring_view text);
    // Per-directory tab colors (dir NormDirKey -> "#RRGGBB"). Persisted so a color follows its
    // working directory across sessions/runs (a color is shared by every tab in that dir).
    std::wstring SerializeDirColors(const std::vector<std::pair<std::wstring, std::wstring>>& colors);
    std::vector<std::pair<std::wstring, std::wstring>> DeserializeDirColors(std::wstring_view text);
    // Manager-tab splitter geometry (pane sizes survive close/reopen). Deserialize clamps.
    std::wstring SerializeLayout(const ManagerLayout& layout);
    ManagerLayout DeserializeLayout(std::wstring_view text);
    // Global app settings (the Settings cog). Deserialize falls back to per-field defaults.
    std::wstring SerializeAppSettings(const AppSettings& settings);
    AppSettings DeserializeAppSettings(std::wstring_view text);
    // A single window's record (M10). One file per window; round-trips geometry + ordered tabs
    // + the Manager lens. Deserialize tolerates a missing/corrupt document (empty record).
    std::wstring SerializeWindowRecord(const WindowRecord& record);
    WindowRecord DeserializeWindowRecord(std::wstring_view text);

    // ---- disk (state dir; best-effort) ----
    void SaveSessions(const std::vector<SessionInfo>& sessions);
    std::vector<SessionInfo> LoadSessions();
    void SaveTemplates(const std::vector<PlanTemplate>& templates);
    std::vector<PlanTemplate> LoadTemplates();
    void SaveRecentDirs(const std::vector<std::wstring>& dirs);
    std::vector<std::wstring> LoadRecentDirs();
    // Replace the saved LIST in model-list.json and KEEP its last specified model: the "last" key is
    // re-read from the freshest file and written back unchanged, so no list rewrite (a Fetch reset,
    // or any future caller) can wipe what the prompt opens with. Best-effort like every state write:
    // a failed write is logged ([persist-fail] in hooks.log) and the old file stays whole.
    //   models - the ids to store, in drop-down order, written exactly as given.
    // Throws std::bad_alloc only.
    void SaveModelList(const std::vector<std::wstring>& models);
    // The saved list, read from disk on every call (another window may have just changed it).
    //   returns - the ids in stored order (newest specified first); empty when the file is missing
    //             or unreadable.
    // Throws std::bad_alloc only.
    std::vector<std::wstring> LoadModelList();
    // The model the user last COMMITTED in the "Specify a model..." prompt — what the prompt pre-fills
    // its box with (AgentBuildSpecifyModelCard). Read from disk on every call, so a commit in one
    // window shows in the next prompt that ANY window opens, and survives a restart.
    //   returns - the trimmed id, or "" when none was ever committed in this profile (or the file is
    //             missing / corrupt); the prompt then opens with an empty box, as it did before.
    // Throws std::bad_alloc only.
    std::wstring LoadLastSpecifiedModel();
    // The one call every "Specify..." commit makes: push `id` to the front of the saved list AND make
    // it the last specified model, in ONE read and ONE write of model-list.json. The freshest file is
    // read first, so two windows committing models can't lose each other's entries. A blank id
    // changes nothing and writes nothing (Default is not a model).
    //   id - the committed model id; trimmed here (' ', '\t', '\r', '\n'), like PushModelListEntry.
    // Throws std::bad_alloc only.
    void RememberSpecifiedModel(std::wstring_view id);
    // The "Fetch models" RESET: replace the whole list with `ids` (order kept, deduped, capped).
    // Destructive by design — Fetch is how a user says "rebuild this from the published lists", so
    // ids they specified earlier are cleared along with a previous fetch's. The configured launch
    // models are unaffected (they live in settings and are merged in at display time). The last
    // specified model is KEPT (SaveModelList preserves it): a fetch is not a model the user chose, so
    // the next prompt still opens with the id they last committed, even when the new list lacks it.
    //   ids - the fetched ids, Anthropic's before Codex's; blanks dropped, case-insensitive repeats
    //         dropped (first spelling kept), capped at kMaxModelListEntries.
    // Throws std::bad_alloc only.
    void ResetModelList(const std::vector<std::wstring>& ids);
    // Agentmaster (PRESETS, doc/agentmaster/HARNESSES.md): the path of <profile>\launch-presets.json —
    // the user's preset list, provider overrides and seeded-template record (LaunchPresets.h owns the
    // format; LaunchPresetsOs.h caches it). Its own file, not an AppSettings field, for the model-list
    // reason above: settings.json is a read-modify-write document several surfaces contend for.
    // Returns the full path. Throws std::bad_alloc only.
    std::wstring LaunchPresetsPath();
    // The raw text of launch-presets.json ("" when the file is missing or unreadable — the loader then
    // reads an empty config and seeds the shipped templates). Throws std::bad_alloc only.
    std::wstring LoadLaunchPresetsText();
    // Write launch-presets.json atomically (temp + rename; a failure is logged as [persist-fail], like
    // every other profile file). Only the Settings cog's Save calls it, so it never races a reader's
    // in-memory seed.
    //   text    - the whole document (LaunchPresetsConfigToJson).
    //   returns - true when the file now holds `text`.
    // Throws std::bad_alloc only.
    bool SaveLaunchPresetsText(std::wstring_view text);
    void SaveLayout(const ManagerLayout& layout);
    ManagerLayout LoadLayout();
    void SaveAppSettings(const AppSettings& settings);
    AppSettings LoadAppSettings();
    // Per-window records under windows/<windowId>.json (M10). Save writes one file (creating
    // the windows/ subdir); Load reads every windows/*.json; Delete removes one. Closing a
    // window never deletes — the sole prune is Kill (Correctness Rule #7).
    void SaveWindowRecord(const WindowRecord& record);
    std::vector<WindowRecord> LoadWindowRecords();
    // Load a SINGLE window record by id (windows/<windowId>.json), or nullopt if absent/corrupt. Used
    // to return a closed window's record to the in-session claim pool (Engine::UnregisterLiveWindow) so
    // the "Reopen Windows" recover button can re-claim it instead of minting a lens-less duplicate.
    std::optional<WindowRecord> LoadWindowRecord(const std::wstring& windowId);
    void DeleteWindowRecord(const std::wstring& windowId);
    // The open-at-exit manifest (open-windows.json, a sibling of sessions.json — NOT under windows/,
    // so it never pollutes the windows/*.json record scan). Save overwrites with the given live
    // window-id set; Load reads it back (empty if absent/corrupt). M10 Increment 3; §13.5.
    void SaveOpenWindows(const std::vector<std::wstring>& windowIds);
    std::vector<std::wstring> LoadOpenWindows();
    // Per-directory tab colors on disk (dir-colors.json). Get/Set are thread-safe load-modify-save
    // convenience over the whole map; Set with nullopt removes the dir's entry (color reset).
    void SaveDirColors(const std::vector<std::pair<std::wstring, std::wstring>>& colors);
    std::vector<std::pair<std::wstring, std::wstring>> LoadDirColors();
    std::optional<std::wstring> GetDirColor(const std::wstring& dir);
    void SetDirColor(const std::wstring& dir, const std::optional<std::wstring>& colorHex);

    // Agentmaster (SIDE_PANEL.md §2c): ONE dir-colors.json snapshot, indexed for a caller that resolves
    // MANY sessions' colors in one pass. GetDirColor (and the per-session ResolveSessionColorHex built on
    // it) stats the file and copies the WHOLE folder->color map on every call — fine for one tab, but
    // O(cards x folders) allocations for a card list (the side panel resolves every card's color on each
    // update). Take one index per pass instead and resolve each session with the 3-argument
    // ResolveSessionColorHex below. A snapshot: a color written after LoadDirColorIndex is not in it, so
    // build a fresh one per pass rather than keeping one around.
    struct DirColorIndex
    {
        std::unordered_map<std::wstring, std::wstring> byKey; // NormDirKey(dir) -> "#RRGGBB", as dir-colors.json stores it

        // The persisted color of `dir` — the answer GetDirColor would give, from the snapshot.
        //   dir     — any spelling of the directory (normalized with NormDirKey here).
        //   returns — "#RRGGBB", or nullopt when the folder has no persisted color.
        //   throws  — std::bad_alloc on allocation failure only.
        std::optional<std::wstring> Find(const std::wstring& dir) const;
    };

    // Read dir-colors.json once (through LoadDirColors' (size,mtime) cache) into a DirColorIndex.
    //   returns — the index; empty when no folder has a color yet (or the file is missing/unreadable).
    //   throws  — std::bad_alloc on allocation failure only.
    DirColorIndex LoadDirColorIndex();

    // Per-directory env overrides on disk (dir-env.json) — a NormDirKey -> env-text map (env-text is
    // the multi-line NAME=VALUE block the Settings cog's Per-directory tab edits). Merged OVER the
    // global AppSettings.env at spawn (MergeSessionEnv / ResolveSessionEnv). Same shape + thread-safe
    // load-modify-save lifecycle as dir-colors.json; the cog is the sole writer. SetDirEnv with a blank
    // block removes the dir's entry.
    std::wstring SerializeDirEnv(const std::vector<std::pair<std::wstring, std::wstring>>& entries);
    std::vector<std::pair<std::wstring, std::wstring>> DeserializeDirEnv(std::wstring_view text);
    void SaveDirEnv(const std::vector<std::pair<std::wstring, std::wstring>>& entries);
    std::vector<std::pair<std::wstring, std::wstring>> LoadDirEnv();
    std::wstring GetDirEnv(const std::wstring& dir);
    void SetDirEnv(const std::wstring& dir, std::wstring_view envText);

    // ---- Claude USER settings.json repository (ENV_VARS.md §8) ----
    // A thin managed layer over the user's GLOBAL Claude settings file — `<CLAUDE_CONFIG_DIR | ~/.claude>
    // /settings.json` — which is DISTINCT from Agentmaster's own settings.json (the cog's AppSettings).
    // Used for `cleanupPeriodDays` (transcript/history retention), surfaced as the cog's "Keep Claude
    // history (days)" field. Every write is a read-modify-write that PRESERVES every key we don't manage,
    // and REFUSES to overwrite a non-empty file it can't parse (so a hand-edited settings.json is never
    // clobbered). Thread-safe.
    // PURE core (no disk; unit-tested): given the current file TEXT, set (or remove, when `value` is
    // nullopt) a top-level NUMBER key, preserving every other key + its order, pretty-printed (2-space).
    // Returns nullopt iff `existing` is non-empty but not a JSON object (caller must NOT overwrite then).
    std::optional<std::wstring> UpsertJsonNumberKey(std::wstring_view existing, std::wstring_view key, std::optional<double> value);
    std::wstring ClaudeUserSettingsPath(); // "<config>/settings.json" ("" if USERPROFILE/CLAUDE_CONFIG_DIR unresolved)
    std::optional<int64_t> GetClaudeCleanupPeriodDays(); // nullopt => key absent / file missing / unreadable
    bool SetClaudeCleanupPeriodDays(std::optional<int64_t> days); // nullopt removes the key; RMW; true on success

    // ---- one-time shipped-default seeding (run once at engine init; ENV_VARS.md §8) ----
    // SeedSessionEnvDefaults: append any not-yet-seeded global env defaults (ApplyEnvDefaults /
    // kEnvDefaultsVersion) to AppSettings.env and bump AppSettings.envDefaultsVersion — so new installs +
    // updaters get them, and a deleted default never returns. No-op once envDefaultsVersion is current.
    void SeedSessionEnvDefaults();
    // SeedClaudeCleanupPeriodDaysIfNeeded: if we've not seeded before (AppSettings.claudeCleanupDaysSeeded)
    // AND the user's global settings.json has no cleanupPeriodDays of their own, write 36500 (~never purge
    // history; 0 is a Claude footgun — it DISABLES persistence) and set the marker. Respects a user value;
    // never re-seeds after the user changes/removes it.
    void SeedClaudeCleanupPeriodDaysIfNeeded();

    // ---- tab naming + per-directory color (pure; testable) ----
    // Derive a tab/session display name from a working directory. First walk up past generic
    // build/output/structural segments (bin/obj/Debug/... the top 20) to the first meaningful
    // folder, then apply the configured NAMING TECHNIQUE (TabTitleNaming: last word (default) /
    // folder name as-is / "<parent>/<folder>" / capitals only / the git branch alone or prefixed —
    // "<branch>/<folder>", "<branch>/<parent>/<folder>"; the branch is opts.branch, an INPUT) +
    // the output transforms (TabTitleCase; whitespace -> '_'; '\' -> '/' unconditionally). The
    // result is never empty ("claude"); as a safety net only, a title past 255 chars trims to
    // 252 + "..." (== 255 total — real names stay whole, the strip visually trims). The 2-arg
    // form is PURE (options passed in — tests + the cog's live example preview); the 1-arg form
    // applies the cog's CURRENT tabTitle* settings (LoadAppSettings — read fresh from disk, so a
    // Save affects the very next launch) and resolves the branch (ReadGitBranchForDir) only when
    // the technique needs it.
    std::wstring DeriveSessionTitle(const std::wstring& workingDir, const TitleNamingOptions& opts);
    std::wstring DeriveSessionTitle(const std::wstring& workingDir);
    // The AppSettings -> TitleNamingOptions bridge (the three tabTitle* fields; branch stays empty
    // — it is per-dir, not a setting: callers needing it resolve ReadGitBranchForDir themselves).
    TitleNamingOptions TitleNamingFromSettings(const AppSettings& s);
    // Whether a technique consumes TitleNamingOptions::branch (the Branch* trio) — the 1-arg
    // derive gates its ReadGitBranchForDir on this, and the cog preview gates its example-branch
    // note on the same predicate (single-source, so they can't drift).
    bool TitleNamingUsesBranch(TabTitleNaming n);
    // Agentmaster (COMMANDS.md): the GENERALIZED suffix-title derivation behind DeriveForkTitle —
    // append " (<word>)" to a source title, and when the source ALREADY ends in that group, BUMP a
    // counter (" (<word> 2)", " (<word> 3)", ...) instead of stacking ("X (fork) (fork)"). Only a
    // trailing " (<word>)" / " (<word> N)" group is recognized (nested/earlier parens left intact);
    // a DIFFERENT word's trailing group is left intact too ("T (fork)" + "handover" ->
    // "T (fork) (handover)" — a handover OF a fork keeps both lineages readable). Pure + testable;
    // "fork" (both fork entry points) and "handover" (the /handover successor tab) share it.
    std::wstring DeriveSuffixedTitle(const std::wstring& sourceTitle, std::wstring_view word);

    // Derive a fork's title from its source's: a first fork appends " (fork)"; forking a fork BUMPS a
    // counter (" (fork 2)", " (fork 3)", ...) instead of stacking suffixes ("X (fork) (fork)"). Only a
    // trailing " (fork)" / " (fork N)" group is recognized (nested/earlier parens are left intact).
    // Pure + testable; shared by both fork entry points (duplicate-tab fork, fork-from-disk).
    // (== DeriveSuffixedTitle(sourceTitle, L"fork").)
    std::wstring DeriveForkTitle(const std::wstring& sourceTitle);
    // Whether a single path segment (any case) is a generic build/output/structural folder name
    // we skip when naming (bin, obj, debug, release, build, ... — the top 20).
    bool IsGenericDirName(const std::wstring& segment);
    // Canonical comparison key for a working directory: separators normalized, trailing slash
    // stripped, and (on Windows) lowercased — so case/slash variants of one dir collapse to one
    // key. Keys the per-directory color map and matches sibling tabs in the same directory.
    std::wstring NormDirKey(const std::wstring& dir);
    // Install the color seed (the Engine calls this once at init with a random value). The seed only
    // randomizes the probe ORDER for a dir's FIRST-ever assignment (which then persists permanently);
    // it never moves an already-assigned color. A test/headless caller may pass a fixed seed for
    // determinism; a caller that touches a color before any seed is set gets a lazy random one.
    void SeedDirColors(uint64_t seed);
    // A directory's auto tab color "#RRGGBB" — PURE PREVIEW, no disk/mutation: the first color of the
    // dir's seeded probe order. Only a representative for a dir with NO persisted color; callers wanting
    // the dir's real (permanent) color check GetDirColor first. The real pick is AssignDirAutoColor.
    std::wstring AutoDirColorHex(const std::wstring& dir);
    // Deal a directory a PERMANENT, collision-free auto color and persist it to dir-colors.json (so the
    // folder keeps that color across tabs/windows/restarts — Rule #12). Prefers a color no other folder
    // already holds; when the palette is exhausted it resets and reuses, still avoiding colors actively
    // shown by an open tab. `openDirKeys` = the NormDirKeys of the currently-open dirs (all windows).
    std::wstring AssignDirAutoColor(const std::wstring& dir, const std::vector<std::wstring>& openDirKeys);
    // PURE core of AssignDirAutoColor (no disk; unit-testable): given the existing folder->color map
    // and the colors currently shown by open tabs, choose dirKey's color (already-assigned wins; else
    // first palette color no folder holds; else — palette exhausted — first not actively shown; else
    // the dir's preferred color). Returns "#RRGGBB".
    std::wstring ChooseDirColor(const std::wstring& dirKey,
                                const std::vector<std::pair<std::wstring, std::wstring>>& existing,
                                const std::unordered_set<std::wstring>& activeColors,
                                uint64_t seed);
    // PURE: de-collide a folder->color map so every palette color is unique across folders, keeping
    // each folder's color where possible (first occurrence wins; duplicates get a free palette color;
    // off-palette user picks are kept verbatim). Used by the v1->v2 migration.
    std::vector<std::pair<std::wstring, std::wstring>>
    DeCollideDirColors(const std::vector<std::pair<std::wstring, std::wstring>>& entries, uint64_t seed);
    // Agentmaster (tab color modes — TabColorMode::Individual): deal a SESSION its own auto tab
    // color. The same collision-avoiding chooser as a dir's (ChooseDirColor), keyed by the session
    // id: the first color of the session's seeded probe order that no other LIVE session already
    // wears (`liveSessionColors` = the (sessionId, "#RRGGBB") pairs of the OTHER open sessions,
    // all windows), then — palette exhausted — the first color no open tab actively shows
    // (`activeColors`), else the session's preferred color. NO disk write here: the caller
    // persists the pick on the SessionInfo record (sessions.json, the per-session permanence),
    // NOT dir-colors.json — the folder<->color map stays folders-only (Rule #12), so Individual
    // mode can't exhaust the dir palette. Deterministic given the engine's color seed.
    std::wstring ChooseSessionAutoColor(const std::wstring& sessionId,
                                        const std::vector<std::pair<std::wstring, std::wstring>>& liveSessionColors,
                                        const std::unordered_set<std::wstring>& activeColors);
    // Agentmaster (inferred working dir — the HOME-DIR forcing): whether the inferred-working-dir
    // machinery applies to THIS session under `mode`. TRUE while the GLOBAL mode is
    // InferredWorkingDirectory (the cog opt-in — every session infers), and ALSO — in EVERY mode —
    // for a session LAUNCHED in the user's home directory (%USERPROFILE%, the launch box's
    // empty-`defaultLaunchDir` fallback): a claude started there almost never WORKS there (the
    // user just opened a tab and ran `claude` without cd-ing), so its cwd is meaningless for
    // grouping/color and the inference is FORCED ON for that tab regardless of the mode. This one
    // predicate gates BOTH the consumers (EffectiveWorkingDir answers the inferred dir only while
    // it holds — a dormant inference from a past Inferred-mode run still never leaks for a
    // deliberately-chosen cwd) AND the producer (_ScanInferredTabColors scans exactly the sessions
    // it admits, so a forced session's inference stays fresh in every mode). Pure apart from the
    // one-time %USERPROFILE% read (cached process-wide; unresolvable env ⇒ no forcing — fails
    // open to the classic cwd behavior).
    bool SessionInfersWorkingDir(TabColorMode mode, const SessionInfo& s);
    // Agentmaster (inferred working dir): THE one answer to "which directory does this session
    // WORK in" — the session's EFFECTIVE working directory. When the session INFERS
    // (SessionInfersWorkingDir — the InferredWorkingDirectory mode, or a home-dir launch in ANY
    // mode) and an inference is known, it is the INFERRED dir (SessionInfo::inferredWorkingDir —
    // where the session's tool calls actually concentrate); every other case (a non-inferring
    // mode on a deliberately-chosen cwd, or no inference yet) it is the launch cwd
    // (SessionInfo::workingDir). Every SEMANTIC "where does this session
    // belong" surface routes through this — Explorer-Tree grouping + dir scope, the board card's
    // dir line + scope filter, the Auto-Testing header + apply-template-to-dir broadcast, the
    // launch-box pre-aim, Open New Session Here, the overlay subline/Open Path/Copy Path — so they
    // can never disagree with the tab color (SessionColorKeyDir delegates here). The raw
    // `s.workingDir` stays the TERMINAL's cwd and keeps owning the mechanics: spawn/resume/fork/
    // restart cwd, hook + transcript correlation (EncodeCwdToProjectDir), per-dir env, persistence.
    // Callers NormDirKey/PathEq it where a canonical key is needed. Pure.
    std::wstring EffectiveWorkingDir(TabColorMode mode, const SessionInfo& s);
    // Agentmaster (tab color modes): the DIR that keys a session's color under `mode` — the
    // color/fan-out key. An INFERRING session (SessionInfersWorkingDir — the Inferred mode, or
    // a home-dir launch in any mode) with a known inference => the inferred dir; everything else
    // (incl. Individual, whose callers branch on the mode BEFORE any dir grouping)
    // => the session's working dir. Callers NormDirKey it where a canonical key is needed.
    // Builds on EffectiveWorkingDir, with ONE deliberate transform: a git WORKTREE effective dir is
    // canonicalized to its MAIN repo root (ProcessInspect::ResolveWorktreeMainRoot, memoized), so a
    // repo and ALL its worktrees share ONE color. This is the SINGLE divergence from
    // EffectiveWorkingDir — which stays worktree-granular for the Explorer-Tree GROUPING + mechanics
    // (a new session still spawns in the worktree; the tree still lists it under the worktree). A
    // non-worktree effective dir is returned unchanged, so color key == EffectiveWorkingDir for every
    // non-worktree case. NOT pure (a one-time, cached git-layout probe per unseen dir).
    std::wstring SessionColorKeyDir(TabColorMode mode, const SessionInfo& s);
    // Agentmaster (tab color modes): READ a session's tab color under `mode` — the one resolution
    // every display surface shares (board title band, Sessions-page chip, pending-dots contrast),
    // so a card/chip always matches what the tab actually wears. NoColor ("Remove colors") =>
    // EMPTY (no surface wears a color; the persisted colors are kept on disk, just not read).
    // Individual => the session's own persisted tabColorHex (falling back to the dir-keyed color
    // while none is dealt yet); dir modes => the key dir's persisted color, else its
    // AutoDirColorHex preview (the existing board/chip precedence). Read-only — never
    // deals/persists (the tab PAINT seams do that: AssignDirAutoColor /
    // TerminalPage::_ApplySessionTabColor's Individual deal).
    std::wstring ResolveSessionColorHex(TabColorMode mode, const SessionInfo& s);
    // The BATCH form (SIDE_PANEL.md §2c): the identical precedence (one shared implementation), with the
    // key dir's persisted color read from `dirColors` — a LoadDirColorIndex snapshot the caller took once
    // for the whole pass — instead of one dir-colors.json stat + full map copy per call.
    //   mode      — the GLOBAL tab-color mode.
    //   s         — the session.
    //   dirColors — this pass's LoadDirColorIndex().
    //   returns   — what the 2-argument form returns for the same session and file contents.
    //   throws    — std::bad_alloc on allocation failure only.
    std::wstring ResolveSessionColorHex(TabColorMode mode, const SessionInfo& s, const DirColorIndex& dirColors);
    // One-time dir-colors.json upgrade (v1 -> v2): de-collide the persisted map (every folder keeps its
    // color where possible; duplicate palette colors are reassigned to free ones). Idempotent (version).
    void MigrateDirColorsToV2IfNeeded();

    // ---- templates: build + apply ----
    // Capture a session's current queue as a reusable template.
    PlanTemplate MakeTemplateFromQueue(const std::wstring& name, const std::vector<QueuedPrompt>& queue);
    // Append a template's prompts to a queue with FRESH ids and status reset to Pending
    // (so applying never accidentally re-marks something Sent). Pure.
    void AppendTemplateToQueue(std::vector<QueuedPrompt>& queue, const PlanTemplate& tmpl);
}
