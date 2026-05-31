// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — persistence (DESIGN §13) + plan templates / apply-to-many (DESIGN §10).
// Sessions (queue + autopilot + metadata) and plan templates serialize to JSON under the
// app state dir. Restore never replays already-Sent prompts (statuses are preserved).
//
// Enum<->string and the struct<->JSON mappings are pure (Json.h only) and unit-tested.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Json.h"
#include "SessionModels.h"

namespace Agentmaster
{
    // ---- enum <-> string ----
    std::wstring ToString(SessionState s);
    SessionState SessionStateFromString(std::wstring_view s);
    std::wstring ToString(AutopilotMode m);
    AutopilotMode AutopilotModeFromString(std::wstring_view s);
    std::wstring ToString(PromptStatus s);
    PromptStatus PromptStatusFromString(std::wstring_view s);
    std::wstring ToString(PromptGate g);
    PromptGate PromptGateFromString(std::wstring_view s);
    std::wstring ToString(PromptOrigin o);
    PromptOrigin PromptOriginFromString(std::wstring_view s);

    // ---- struct <-> json::Value ----
    json::Value ToJson(const QueuedPrompt& p);
    QueuedPrompt PromptFromJson(const json::Value& v);
    json::Value ToJson(const AutopilotState& a);
    AutopilotState AutopilotFromJson(const json::Value& v);
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

    // ---- tab naming + per-directory color (pure; testable) ----
    // Derive a tab/session display name from a working directory: walk up past generic build/
    // output/structural segments (bin/obj/Debug/... the top 20) to the first meaningful folder,
    // then apply the length rules — <=16 chars used as-is; >16 mixed-case -> its capital letters
    // only; >16 all-lowercase -> as-is, truncated past 30 chars with "...". Never empty ("claude").
    std::wstring DeriveSessionTitle(const std::wstring& workingDir);
    // Whether a single path segment (any case) is a generic build/output/structural folder name
    // we skip when naming (bin, obj, debug, release, build, ... — the top 20).
    bool IsGenericDirName(const std::wstring& segment);
    // Canonical comparison key for a working directory: separators normalized, trailing slash
    // stripped, and (on Windows) lowercased — so case/slash variants of one dir collapse to one
    // key. Keys the per-directory color map and matches sibling tabs in the same directory.
    std::wstring NormDirKey(const std::wstring& dir);
    // A stable, pleasant default tab color ("#RRGGBB") for a directory, chosen from a fixed palette
    // by hashing NormDirKey(dir) — same dir => same color; different dirs spread across the palette.
    std::wstring AutoDirColorHex(const std::wstring& dir);

    // ---- templates: build + apply ----
    // Capture a session's current queue as a reusable template.
    PlanTemplate MakeTemplateFromQueue(const std::wstring& name, const std::vector<QueuedPrompt>& queue);
    // Append a template's prompts to a queue with FRESH ids and status reset to Pending
    // (so applying never accidentally re-marks something Sent). Pure.
    void AppendTemplateToQueue(std::vector<QueuedPrompt>& queue, const PlanTemplate& tmpl);
}
