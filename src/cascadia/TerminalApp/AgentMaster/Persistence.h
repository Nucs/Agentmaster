// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — persistence (DESIGN §13) + plan templates / apply-to-many (DESIGN §10).
// Sessions (queue + autopilot + metadata) and plan templates serialize to JSON under the
// app state dir. Restore never replays already-Sent prompts (statuses are preserved).
//
// Enum<->string and the struct<->JSON mappings are pure (Json.h only) and unit-tested.

#pragma once

#include <string>
#include <string_view>
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
    void DeleteWindowRecord(const std::wstring& windowId);

    // ---- templates: build + apply ----
    // Capture a session's current queue as a reusable template.
    PlanTemplate MakeTemplateFromQueue(const std::wstring& name, const std::vector<QueuedPrompt>& queue);
    // Append a template's prompts to a queue with FRESH ids and status reset to Pending
    // (so applying never accidentally re-marks something Sent). Pure.
    void AppendTemplateToQueue(std::vector<QueuedPrompt>& queue, const PlanTemplate& tmpl);
}
