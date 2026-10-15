// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session fold (see SessionDbFold.h).

#include "SessionDbFold.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "DuckDbWrap.h" // ToUtf8 / FromUtf8

namespace Agentmaster::SessionDb
{
    namespace
    {
        // Bounds that keep a fold small whatever a transcript does (a session that visits a thousand
        // folders keeps the first 256 distinct keys; its count stays exact up to that bound).
        constexpr size_t kMaxDistinctCwds = 256;
        constexpr size_t kMaxDistinctBranches = 64;
        constexpr size_t kMaxDistinctModels = 64;
        constexpr size_t kMaxDistinctNames = 128; // skills, MCP servers
        constexpr size_t kMaxPendingTools = 2000; // calls whose result never came (cut-off turns)
        constexpr size_t kMaxPreviewChars = 300; // last_prompt / last_reply

        // Claude's terminal stop reasons (SessionScanner.h IsTerminalStopReason, restated: that header is
        // the scanner's and pulls the whole state machine).
        bool IsTerminalStop(const std::wstring& r)
        {
            return r == L"end_turn" || r == L"stop_sequence" || r == L"max_tokens" || r == L"refusal";
        }

        // Add `v` to a first-seen-ordered distinct list (exact match), bounded.
        void AddDistinct(std::vector<std::wstring>& list, const std::wstring& v, size_t cap)
        {
            if (v.empty() || list.size() >= cap || std::find(list.begin(), list.end(), v) != list.end())
            {
                return;
            }
            list.push_back(v);
        }

        // TranscriptStore's title shape (FirstLineTrim) over the first `cap` characters — what the sidecar
        // stores as firstUserPrompt (it caps the prompt at 2048 before trimming).
        std::wstring PreviewLine(const std::wstring& s, size_t cap)
        {
            const std::wstring head = s.substr(0, std::min(s.size(), cap));
            size_t i = 0;
            while (i < head.size() && (head[i] == L'\n' || head[i] == L'\r' || head[i] == L' ' || head[i] == L'\t'))
            {
                ++i;
            }
            size_t j = i;
            while (j < head.size() && head[j] != L'\n' && head[j] != L'\r')
            {
                ++j;
            }
            while (j > i && (head[j - 1] == L' ' || head[j - 1] == L'\t'))
            {
                --j;
            }
            return head.substr(i, j - i);
        }

        using json = nlohmann::json;

        // wstring <-> JSON string helpers (UTF-8 inside JSON).
        json W(const std::wstring& s)
        {
            return ToUtf8(s);
        }
        std::wstring S(const json& j, const char* key)
        {
            const auto it = j.find(key);
            return (it != j.end() && it->is_string()) ? FromUtf8(it->get_ref<const std::string&>()) : std::wstring{};
        }
        int64_t I(const json& j, const char* key)
        {
            const auto it = j.find(key);
            return (it != j.end() && it->is_number()) ? it->get<int64_t>() : 0;
        }
        double D(const json& j, const char* key)
        {
            const auto it = j.find(key);
            return (it != j.end() && it->is_number()) ? it->get<double>() : 0.0;
        }
        bool B(const json& j, const char* key)
        {
            const auto it = j.find(key);
            return it != j.end() && it->is_boolean() && it->get<bool>();
        }
        json WList(const std::vector<std::wstring>& v)
        {
            json a = json::array();
            for (const auto& s : v)
            {
                a.push_back(ToUtf8(s));
            }
            return a;
        }
        std::vector<std::wstring> SList(const json& j, const char* key)
        {
            std::vector<std::wstring> out;
            const auto it = j.find(key);
            if (it != j.end() && it->is_array())
            {
                for (const auto& e : *it)
                {
                    if (e.is_string())
                    {
                        out.push_back(FromUtf8(e.get_ref<const std::string&>()));
                    }
                }
            }
            return out;
        }
    }

    void ApplyLine(SessionFold& f, const LineFacts& x, int32_t lineNo, std::wstring (*normDir)(const std::wstring&))
    {
        ++f.lines;
        if (!x.json)
        {
            return;
        }
        const bool sidechain = (x.flags & kLfSidechain) != 0;
        const bool isAssistant = x.type == L"assistant";
        const bool isUser = x.type == L"user";

        // --- places ---
        if (!x.cwd.empty())
        {
            if (f.cwdFirst.empty())
            {
                f.cwdFirst = x.cwd; // TranscriptStats' rule: the first cwd seen on any line
            }
            f.cwdLast = x.cwd;
            AddDistinct(f.cwdKeys, normDir ? normDir(x.cwd) : x.cwd, kMaxDistinctCwds);
        }
        if (!x.relocatedCwd.empty())
        {
            f.relocatedCwd = x.relocatedCwd;
        }
        if (x.worktree)
        {
            f.worktreePath = x.worktreePath;
            f.worktreeName = x.worktreeName;
            f.worktreeBranch = x.worktreeBranch;
            f.worktreeOriginalCwd = x.worktreeOriginalCwd;
        }
        if (x.environment && !f.env)
        {
            f.env = true; // the FIRST environment snapshot (the session's starting place)
            f.envIsGitRepo = x.envIsGitRepo;
            f.envIsWorktree = x.envIsWorktree;
            f.envShell = x.envShell;
            f.envExtraDirs = x.envExtraDirs;
        }

        // --- identity, versions, models ---
        if (!x.branch.empty())
        {
            if (f.branchFirst.empty())
            {
                f.branchFirst = x.branch;
            }
            f.branchLast = x.branch;
            AddDistinct(f.branches, x.branch, kMaxDistinctBranches);
        }
        if (!x.version.empty())
        {
            if (f.versionFirst.empty())
            {
                f.versionFirst = x.version;
            }
            f.versionLast = x.version;
        }
        if (!x.model.empty())
        {
            f.modelLast = x.model;
            AddDistinct(f.models, x.model, kMaxDistinctModels);
        }
        if (!x.effort.empty())
        {
            f.effortLast = x.effort;
        }
        if (f.entrypoint.empty() && !x.entrypoint.empty())
        {
            f.entrypoint = x.entrypoint;
        }
        if (!x.permissionMode.empty())
        {
            f.permissionModeLast = x.permissionMode;
        }
        if (x.type == L"mode" && x.mode == L"plan")
        {
            ++f.planModeMarks;
        }
        if (!x.agentName.empty())
        {
            f.agentName = x.agentName;
        }

        // --- times (TranscriptStats' rules for first / last activity) ---
        if (x.tsMs != 0 && f.firstTsMs == 0)
        {
            f.firstTsMs = x.tsMs;
        }
        if (!sidechain && x.tsMs != 0 && (isUser || isAssistant))
        {
            f.lastActivityMs = std::max(f.lastActivityMs, x.tsMs); // max: a fork's copied tail carries OLD stamps
        }
        if (x.typedPrompt && x.tsMs != 0)
        {
            f.lastTypedMs = std::max(f.lastTypedMs, x.tsMs);
        }
        if (x.tsMs != 0 && ((isAssistant && IsTerminalStop(x.stopReason)) || x.interruptMarker))
        {
            f.lastTurnEndMs = std::max(f.lastTurnEndMs, x.tsMs);
        }
        if (x.turnDurationMs >= 0)
        {
            ++f.turnsTimed;
            f.turnMsTotal += x.turnDurationMs;
            f.turnMsMax = std::max(f.turnMsMax, x.turnDurationMs);
        }

        // --- Claude Code's own cost record: the LAST one holds the running totals ---
        if (x.costState)
        {
            f.costState = true;
            f.costUsd = x.costUsd;
            f.apiMs = x.apiMs;
            f.apiMsNoRetries = x.apiMsNoRetries;
            f.toolMs = x.toolMs;
            f.wallMs = x.wallMs;
            f.linesAdded = x.linesAdded;
            f.linesRemoved = x.linesRemoved;
            f.costUnknownModel = x.costUnknownModel;
        }

        // --- context (the Ctx column: the newest wins, a compaction resets it) ---
        if (!sidechain && x.contextTokens > 0 && (isAssistant || x.type == L"system"))
        {
            f.contextTokens = x.contextTokens;
        }
        if (!sidechain && x.inputContextTokens > 0)
        {
            f.contextMax = std::max(f.contextMax, x.inputContextTokens);
        }

        // --- outcomes ---
        if (isAssistant && (x.flags & kLfApiError))
        {
            ++f.apiErrors;
            if (!x.apiErrorStatus.empty())
            {
                f.apiErrorStatusLast = x.apiErrorStatus;
            }
        }
        if (isAssistant && x.stopReason == L"refusal")
        {
            ++f.refusals;
        }
        if (isAssistant && x.stopReason == L"max_tokens")
        {
            ++f.maxTokenStops;
        }
        if (x.abortedMidStream)
        {
            ++f.aborted;
        }
        if (x.interruptMarker)
        {
            ++f.interrupts;
        }
        if (x.stopHookMsTotal >= 0)
        {
            f.stopHookMsTotal += x.stopHookMsTotal;
            f.stopHookMsMax = std::max(f.stopHookMsMax, x.stopHookMsMax);
        }

        // --- work done ---
        if (x.typedPrompt)
        {
            ++f.promptsTyped;
            if (f.firstPrompt.empty())
            {
                f.firstPrompt = PreviewLine(x.promptText, 2048); // the sidecar's firstUserPrompt
            }
            f.lastPrompt = PreviewLine(x.promptText, kMaxPreviewChars);
        }
        if (x.queuedPrompt)
        {
            ++f.promptsQueued;
        }
        if (isAssistant && !sidechain)
        {
            ++f.assistantLines;
            f.toolCalls += static_cast<int32_t>(x.toolNames.size());
            for (const auto& n : x.toolNames)
            {
                ++f.toolCallsByTool[n.empty() ? L"(unnamed)" : n];
            }
            if (!x.replyText.empty())
            {
                f.lastReply = PreviewLine(x.replyText, kMaxPreviewChars);
            }
        }
        if (!x.attributionSkill.empty())
        {
            AddDistinct(f.skillsUsed, x.attributionSkill, kMaxDistinctNames);
        }
        for (const auto& tu : x.toolUses)
        {
            if (tu.name == L"Skill" && !tu.skill.empty())
            {
                AddDistinct(f.skillsUsed, tu.skill, kMaxDistinctNames);
            }
        }
        if (!x.attributionMcpServer.empty())
        {
            AddDistinct(f.mcpServers, x.attributionMcpServer, kMaxDistinctNames);
        }
        f.subagentsSpawned += x.subagentsSpawned;
        f.imagesPasted += x.imagesPasted;
        if (x.flags & kLfCompactBoundary)
        {
            ++f.compactions;
            if (x.compactTrigger == L"auto")
            {
                ++f.compactionsAuto;
            }
            f.compactionPreTokensMax = std::max(f.compactionPreTokensMax, x.compactPreTokens);
        }
        if (!x.recap.empty())
        {
            ++f.recaps;
            f.recapLast = x.recap;
        }
        if (x.frameLink)
        {
            ++f.artifacts;
            f.artifactTitle = x.frameTitle;
            f.artifactUrl = x.frameUrl;
        }

        // --- titles (the last line of each kind wins, as in TranscriptStats) ---
        if (!x.customTitle.empty())
        {
            f.customTitle = x.customTitle;
        }
        if (!x.aiTitle.empty())
        {
            f.aiTitle = x.aiTitle;
        }
        if (!x.legacySummary.empty())
        {
            f.legacySummary = x.legacySummary;
        }

        // --- lineage ---
        if (!x.forkedFrom.empty())
        {
            if (f.forkedFrom.empty())
            {
                f.forkedFrom = x.forkedFrom;
            }
            f.forkPointLine = lineNo; // the last copied line so far
        }

        // --- the tool calls awaiting a result, and the end state ---
        for (const auto& tu : x.toolUses)
        {
            if (tu.id.empty())
            {
                continue;
            }
            if (f.pendingTools.size() >= kMaxPendingTools)
            {
                f.pendingTools.erase(f.pendingTools.begin()); // a call whose result never came: forget one
            }
            f.pendingTools[tu.id] = tu.name;
            if (tu.name == L"AskUserQuestion")
            {
                f.pendingAskId = tu.id;
            }
        }
        if (isAssistant && !sidechain)
        {
            if (!x.stopReason.empty())
            {
                f.lastStopReason = x.stopReason;
            }
            f.lastWasInterrupt = false;
        }
        if (x.interruptMarker)
        {
            f.lastWasInterrupt = true;
        }
    }

    std::wstring TakeToolName(SessionFold& f, const std::wstring& toolUseId)
    {
        if (toolUseId.empty())
        {
            return {};
        }
        const auto it = f.pendingTools.find(toolUseId);
        if (it == f.pendingTools.end())
        {
            return {};
        }
        std::wstring name = std::move(it->second);
        f.pendingTools.erase(it);
        if (toolUseId == f.pendingAskId)
        {
            f.pendingAskId.clear(); // the question was answered
        }
        return name;
    }

    std::wstring EndState(const SessionFold& f)
    {
        if (f.lastStopReason.empty() && !f.lastWasInterrupt && f.assistantLines == 0)
        {
            return {};
        }
        if (!f.pendingAskId.empty())
        {
            return L"question";
        }
        if (f.lastWasInterrupt)
        {
            return L"interrupted";
        }
        return IsTerminalStop(f.lastStopReason) ? L"completed" : L"unfinished";
    }

    std::string FoldToJson(const SessionFold& f)
    {
        json j;
        j["v"] = 1;
        j["lines"] = f.lines;
        j["cwdFirst"] = W(f.cwdFirst);
        j["cwdLast"] = W(f.cwdLast);
        j["cwdKeys"] = WList(f.cwdKeys);
        j["relocatedCwd"] = W(f.relocatedCwd);
        j["worktreePath"] = W(f.worktreePath);
        j["worktreeName"] = W(f.worktreeName);
        j["worktreeBranch"] = W(f.worktreeBranch);
        j["worktreeOriginalCwd"] = W(f.worktreeOriginalCwd);
        j["env"] = f.env;
        j["envIsGitRepo"] = f.envIsGitRepo;
        j["envIsWorktree"] = f.envIsWorktree;
        j["envShell"] = W(f.envShell);
        j["envExtraDirs"] = f.envExtraDirs;
        j["branchFirst"] = W(f.branchFirst);
        j["branchLast"] = W(f.branchLast);
        j["branches"] = WList(f.branches);
        j["versionFirst"] = W(f.versionFirst);
        j["versionLast"] = W(f.versionLast);
        j["models"] = WList(f.models);
        j["modelLast"] = W(f.modelLast);
        j["effortLast"] = W(f.effortLast);
        j["entrypoint"] = W(f.entrypoint);
        j["permissionModeLast"] = W(f.permissionModeLast);
        j["agentName"] = W(f.agentName);
        j["planModeMarks"] = f.planModeMarks;
        j["firstTsMs"] = f.firstTsMs;
        j["lastActivityMs"] = f.lastActivityMs;
        j["lastTypedMs"] = f.lastTypedMs;
        j["lastTurnEndMs"] = f.lastTurnEndMs;
        j["turnsTimed"] = f.turnsTimed;
        j["turnMsTotal"] = f.turnMsTotal;
        j["turnMsMax"] = f.turnMsMax;
        j["costState"] = f.costState;
        j["costUsd"] = f.costUsd;
        j["apiMs"] = f.apiMs;
        j["apiMsNoRetries"] = f.apiMsNoRetries;
        j["toolMs"] = f.toolMs;
        j["wallMs"] = f.wallMs;
        j["linesAdded"] = f.linesAdded;
        j["linesRemoved"] = f.linesRemoved;
        j["costUnknownModel"] = f.costUnknownModel;
        j["contextTokens"] = f.contextTokens;
        j["contextMax"] = f.contextMax;
        j["apiErrors"] = f.apiErrors;
        j["apiErrorStatusLast"] = W(f.apiErrorStatusLast);
        j["refusals"] = f.refusals;
        j["maxTokenStops"] = f.maxTokenStops;
        j["aborted"] = f.aborted;
        j["interrupts"] = f.interrupts;
        j["stopHookMsTotal"] = f.stopHookMsTotal;
        j["stopHookMsMax"] = f.stopHookMsMax;
        j["promptsTyped"] = f.promptsTyped;
        j["promptsQueued"] = f.promptsQueued;
        j["assistantLines"] = f.assistantLines;
        j["toolCalls"] = f.toolCalls;
        json byTool = json::object();
        for (const auto& [name, n] : f.toolCallsByTool)
        {
            byTool[ToUtf8(name)] = n;
        }
        j["toolCallsByTool"] = std::move(byTool);
        j["skillsUsed"] = WList(f.skillsUsed);
        j["mcpServers"] = WList(f.mcpServers);
        j["subagentsSpawned"] = f.subagentsSpawned;
        j["imagesPasted"] = f.imagesPasted;
        j["compactions"] = f.compactions;
        j["compactionsAuto"] = f.compactionsAuto;
        j["compactionPreTokensMax"] = f.compactionPreTokensMax;
        j["recaps"] = f.recaps;
        j["recapLast"] = W(f.recapLast);
        j["artifacts"] = f.artifacts;
        j["artifactTitle"] = W(f.artifactTitle);
        j["artifactUrl"] = W(f.artifactUrl);
        j["customTitle"] = W(f.customTitle);
        j["aiTitle"] = W(f.aiTitle);
        j["legacySummary"] = W(f.legacySummary);
        j["firstPrompt"] = W(f.firstPrompt);
        j["lastPrompt"] = W(f.lastPrompt);
        j["lastReply"] = W(f.lastReply);
        j["forkedFrom"] = W(f.forkedFrom);
        j["forkPointLine"] = f.forkPointLine;
        j["lastStopReason"] = W(f.lastStopReason);
        j["lastWasInterrupt"] = f.lastWasInterrupt;
        j["pendingAskId"] = W(f.pendingAskId);
        json pending = json::object();
        for (const auto& [id, name] : f.pendingTools)
        {
            pending[ToUtf8(id)] = ToUtf8(name);
        }
        j["pendingTools"] = std::move(pending);
        return j.dump();
    }

    bool FoldFromJson(const std::string& text, SessionFold& out)
    {
        out = SessionFold{};
        if (text.empty())
        {
            return false;
        }
        const json j = json::parse(text, nullptr, /*allow_exceptions*/ false);
        if (!j.is_object() || I(j, "v") != 1)
        {
            return false; // malformed, or a fold written by another version: the caller re-ingests
        }
        out.lines = I(j, "lines");
        out.cwdFirst = S(j, "cwdFirst");
        out.cwdLast = S(j, "cwdLast");
        out.cwdKeys = SList(j, "cwdKeys");
        out.relocatedCwd = S(j, "relocatedCwd");
        out.worktreePath = S(j, "worktreePath");
        out.worktreeName = S(j, "worktreeName");
        out.worktreeBranch = S(j, "worktreeBranch");
        out.worktreeOriginalCwd = S(j, "worktreeOriginalCwd");
        out.env = B(j, "env");
        out.envIsGitRepo = B(j, "envIsGitRepo");
        out.envIsWorktree = B(j, "envIsWorktree");
        out.envShell = S(j, "envShell");
        out.envExtraDirs = static_cast<int32_t>(I(j, "envExtraDirs"));
        out.branchFirst = S(j, "branchFirst");
        out.branchLast = S(j, "branchLast");
        out.branches = SList(j, "branches");
        out.versionFirst = S(j, "versionFirst");
        out.versionLast = S(j, "versionLast");
        out.models = SList(j, "models");
        out.modelLast = S(j, "modelLast");
        out.effortLast = S(j, "effortLast");
        out.entrypoint = S(j, "entrypoint");
        out.permissionModeLast = S(j, "permissionModeLast");
        out.agentName = S(j, "agentName");
        out.planModeMarks = static_cast<int32_t>(I(j, "planModeMarks"));
        out.firstTsMs = I(j, "firstTsMs");
        out.lastActivityMs = I(j, "lastActivityMs");
        out.lastTypedMs = I(j, "lastTypedMs");
        out.lastTurnEndMs = I(j, "lastTurnEndMs");
        out.turnsTimed = static_cast<int32_t>(I(j, "turnsTimed"));
        out.turnMsTotal = I(j, "turnMsTotal");
        out.turnMsMax = I(j, "turnMsMax");
        out.costState = B(j, "costState");
        out.costUsd = D(j, "costUsd");
        out.apiMs = I(j, "apiMs");
        out.apiMsNoRetries = I(j, "apiMsNoRetries");
        out.toolMs = I(j, "toolMs");
        out.wallMs = I(j, "wallMs");
        out.linesAdded = I(j, "linesAdded");
        out.linesRemoved = I(j, "linesRemoved");
        out.costUnknownModel = B(j, "costUnknownModel");
        out.contextTokens = I(j, "contextTokens");
        out.contextMax = I(j, "contextMax");
        out.apiErrors = static_cast<int32_t>(I(j, "apiErrors"));
        out.apiErrorStatusLast = S(j, "apiErrorStatusLast");
        out.refusals = static_cast<int32_t>(I(j, "refusals"));
        out.maxTokenStops = static_cast<int32_t>(I(j, "maxTokenStops"));
        out.aborted = static_cast<int32_t>(I(j, "aborted"));
        out.interrupts = static_cast<int32_t>(I(j, "interrupts"));
        out.stopHookMsTotal = I(j, "stopHookMsTotal");
        out.stopHookMsMax = I(j, "stopHookMsMax");
        out.promptsTyped = static_cast<int32_t>(I(j, "promptsTyped"));
        out.promptsQueued = static_cast<int32_t>(I(j, "promptsQueued"));
        out.assistantLines = static_cast<int32_t>(I(j, "assistantLines"));
        out.toolCalls = static_cast<int32_t>(I(j, "toolCalls"));
        if (const auto it = j.find("toolCallsByTool"); it != j.end() && it->is_object())
        {
            for (const auto& [k, v] : it->items())
            {
                if (v.is_number())
                {
                    out.toolCallsByTool[FromUtf8(k)] = v.get<int32_t>();
                }
            }
        }
        out.skillsUsed = SList(j, "skillsUsed");
        out.mcpServers = SList(j, "mcpServers");
        out.subagentsSpawned = static_cast<int32_t>(I(j, "subagentsSpawned"));
        out.imagesPasted = static_cast<int32_t>(I(j, "imagesPasted"));
        out.compactions = static_cast<int32_t>(I(j, "compactions"));
        out.compactionsAuto = static_cast<int32_t>(I(j, "compactionsAuto"));
        out.compactionPreTokensMax = I(j, "compactionPreTokensMax");
        out.recaps = static_cast<int32_t>(I(j, "recaps"));
        out.recapLast = S(j, "recapLast");
        out.artifacts = static_cast<int32_t>(I(j, "artifacts"));
        out.artifactTitle = S(j, "artifactTitle");
        out.artifactUrl = S(j, "artifactUrl");
        out.customTitle = S(j, "customTitle");
        out.aiTitle = S(j, "aiTitle");
        out.legacySummary = S(j, "legacySummary");
        out.firstPrompt = S(j, "firstPrompt");
        out.lastPrompt = S(j, "lastPrompt");
        out.lastReply = S(j, "lastReply");
        out.forkedFrom = S(j, "forkedFrom");
        out.forkPointLine = static_cast<int32_t>(I(j, "forkPointLine"));
        out.lastStopReason = S(j, "lastStopReason");
        out.lastWasInterrupt = B(j, "lastWasInterrupt");
        out.pendingAskId = S(j, "pendingAskId");
        if (const auto it = j.find("pendingTools"); it != j.end() && it->is_object())
        {
            for (const auto& [k, v] : it->items())
            {
                if (v.is_string())
                {
                    out.pendingTools[FromUtf8(k)] = FromUtf8(v.get_ref<const std::string&>());
                }
            }
        }
        return true;
    }
}
