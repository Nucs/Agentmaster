// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
#include "Persistence.h"

#include "ClaudeSpawn.h" // AgentmasterStateDir, NewSessionId

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <unordered_set>

namespace
{
    using namespace Agentmaster;

    std::string Utf16ToUtf8(std::wstring_view s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
        {
            return {};
        }
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring Utf8ToUtf16(const std::string& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    bool WriteAllUtf8(const std::wstring& path, std::wstring_view content)
    {
        // The single chokepoint for EVERY persistence write (sessions.json, windows/<id>.json, templates,
        // recent-dirs, open-windows, dir-colors, dir-env, layout, settings). CRASH- AND POWER-LOSS-SAFE.
        //
        // A sudden shutdown must never leave a torn / truncated / zero-length file, because the loaders
        // fail OPEN: a corrupt sessions.json deserializes to an EMPTY fleet, a corrupt windows/<id>.json to
        // an empty record -- silently discarding state the user never closed (Correctness Rule #16). A
        // truncate-in-place ofstream did exactly that on any write interrupted mid-flight. Instead:
        //   1) write the FULL bytes to a sibling temp file,
        //   2) force them to the platter (FlushFileBuffers) so a power loss can't commit the rename while
        //      the data blocks are still unwritten (which would swap in a zero/garbage file),
        //   3) ATOMICALLY swap the temp over the real file (MoveFileExW REPLACE_EXISTING | WRITE_THROUGH --
        //      on NTFS a same-directory rename is atomic, so a reader sees either the whole old file or the
        //      whole new one, never a torn mix, and the old file is left FULLY INTACT if anything fails).
        // Mirrors the engine's other atomic writers (SessionStore::AtomicWriteUtf8 /
        // TranscriptStore::WriteFileUtf8), plus the flush + WRITE_THROUGH for power-loss durability.
        //
        // A failure here = state silently NOT saved, so surface it ([persist-fail]); rare (disk full /
        // permissions / a sharing violation on the destination), so it never floods steady state.
        try
        {
            const auto bytes = Utf16ToUtf8(content);
            // Per-thread temp name: SaveSessions fires from multiple engine threads (the registry
            // observers), so two concurrent writes to the SAME file must not collide on one temp path. A
            // leftover temp from a crashed write is harmless -- its extension is never ".json", so the
            // windows/ scans (LoadWindowRecords + the Emperor reopen) skip it, and CREATE_ALWAYS reuses it.
            // Same directory as the target, so the rename is a same-volume (atomic) metadata move.
            const std::wstring tmp = path + L".tmp." + std::to_wstring(::GetCurrentThreadId());
            const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                AppendStateLog(L"hooks.log", L"[persist-fail] " + path + L" (temp open failed \x2014 state NOT saved)\n");
                return false;
            }
            DWORD wrote = 0;
            const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
            // Flush the data to disk BEFORE the rename. Best-effort: a flush that fails (a quirky FS) must
            // not discard an otherwise-good save -- the atomic rename below still guarantees no torn read.
            if (ok)
            {
                ::FlushFileBuffers(h);
            }
            ::CloseHandle(h);
            if (!ok || wrote != bytes.size())
            {
                AppendStateLog(L"hooks.log", L"[persist-fail] " + path + L" (write incomplete \x2014 state NOT saved)\n");
                ::DeleteFileW(tmp.c_str());
                return false;
            }
            // Atomic swap. REPLACE_EXISTING covers both "first write" and "overwrite"; WRITE_THROUGH flushes
            // the rename metadata before returning. On failure the original file is untouched (never torn).
            if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                AppendStateLog(L"hooks.log", L"[persist-fail] " + path + L" (atomic replace failed \x2014 state NOT saved)\n");
                ::DeleteFileW(tmp.c_str()); // don't leave the temp behind; the old file stays intact
                return false;
            }
            return true;
        }
        catch (...)
        {
            AppendStateLog(L"hooks.log", L"[persist-fail] " + path + L" (exception \x2014 state NOT saved)\n");
            return false;
        }
    }

    std::wstring ReadAllUtf8(const std::wstring& path)
    {
        try
        {
            std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
            if (!f)
            {
                return {};
            }
            std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return Utf8ToUtf16(bytes);
        }
        catch (...)
        {
            return {};
        }
    }
}

namespace Agentmaster
{
    // ---- enum <-> string ----

    std::wstring ToString(SessionState s)
    {
        switch (s)
        {
        case SessionState::Running:
            return L"Running";
        case SessionState::WaitingForInput:
            return L"WaitingForInput";
        case SessionState::NeedsApproval:
            return L"NeedsApproval";
        case SessionState::Error:
            return L"Error";
        case SessionState::Done:
            return L"Done";
        case SessionState::Idle:
        default:
            return L"Idle";
        }
    }
    SessionState SessionStateFromString(std::wstring_view s)
    {
        if (s == L"Running")
            return SessionState::Running;
        if (s == L"WaitingForInput")
            return SessionState::WaitingForInput;
        if (s == L"NeedsApproval")
            return SessionState::NeedsApproval;
        if (s == L"Error")
            return SessionState::Error;
        if (s == L"Done")
            return SessionState::Done;
        return SessionState::Idle;
    }

    std::wstring ToString(AutorunnerMode m)
    {
        switch (m)
        {
        case AutorunnerMode::SemiAuto:
            return L"SemiAuto";
        case AutorunnerMode::Full:
            return L"Full";
        case AutorunnerMode::Off:
        default:
            return L"Off";
        }
    }
    AutorunnerMode AutorunnerModeFromString(std::wstring_view s)
    {
        if (s == L"SemiAuto")
            return AutorunnerMode::SemiAuto;
        if (s == L"Full")
            return AutorunnerMode::Full;
        return AutorunnerMode::Off;
    }

    std::wstring ToString(ExplorerSort s)
    {
        switch (s)
        {
        case ExplorerSort::Oldest:
            return L"oldest";
        case ExplorerSort::MostActive:
            return L"active";
        case ExplorerSort::Alpha:
            return L"alpha";
        case ExplorerSort::ByPid:
            return L"pid";
        case ExplorerSort::Newest:
        default:
            return L"newest";
        }
    }
    ExplorerSort ExplorerSortFromString(std::wstring_view s)
    {
        if (s == L"oldest")
            return ExplorerSort::Oldest;
        if (s == L"active")
            return ExplorerSort::MostActive;
        if (s == L"alpha")
            return ExplorerSort::Alpha;
        if (s == L"pid")
            return ExplorerSort::ByPid;
        return ExplorerSort::Newest;
    }

    std::wstring ToString(TabRenameCommitMode m)
    {
        switch (m)
        {
        case TabRenameCommitMode::ClickAwayOnly:
            return L"clickAway";
        case TabRenameCommitMode::ClickAwayOrEnter:
            return L"enter";
        case TabRenameCommitMode::ClickAwayOrShiftEnter:
        default:
            return L"shiftEnter";
        }
    }
    TabRenameCommitMode TabRenameCommitModeFromString(std::wstring_view s)
    {
        if (s == L"clickAway")
            return TabRenameCommitMode::ClickAwayOnly;
        if (s == L"enter")
            return TabRenameCommitMode::ClickAwayOrEnter;
        return TabRenameCommitMode::ClickAwayOrShiftEnter;
    }

    std::wstring ToString(FavoriteIcon i)
    {
        switch (i)
        {
        case FavoriteIcon::Star:
            return L"star";
        case FavoriteIcon::Crown:
        default:
            return L"crown";
        }
    }
    FavoriteIcon FavoriteIconFromString(std::wstring_view s)
    {
        if (s == L"star")
            return FavoriteIcon::Star;
        return FavoriteIcon::Crown; // default + unknown token -> Crown (the prior behavior)
    }

    std::wstring ToString(TabColorMode m)
    {
        switch (m)
        {
        case TabColorMode::Individual:
            return L"individual";
        case TabColorMode::InferredWorkingDirectory:
            return L"inferredWorkingDirectory";
        case TabColorMode::WorkingDirectory:
        default:
            return L"workingDirectory";
        }
    }
    TabColorMode TabColorModeFromString(std::wstring_view s)
    {
        if (s == L"individual")
            return TabColorMode::Individual;
        if (s == L"inferredWorkingDirectory")
            return TabColorMode::InferredWorkingDirectory;
        return TabColorMode::WorkingDirectory; // default + unknown token -> the prior (per-dir) behavior
    }

    std::wstring ToString(PromptStatus s)
    {
        switch (s)
        {
        case PromptStatus::Sent:
            return L"Sent";
        case PromptStatus::Held:
            return L"Held";
        case PromptStatus::Skipped:
            return L"Skipped";
        case PromptStatus::Failed:
            return L"Failed";
        case PromptStatus::Pending:
        default:
            return L"Pending";
        }
    }
    PromptStatus PromptStatusFromString(std::wstring_view s)
    {
        if (s == L"Sent")
            return PromptStatus::Sent;
        if (s == L"Held")
            return PromptStatus::Held;
        if (s == L"Skipped")
            return PromptStatus::Skipped;
        if (s == L"Failed")
            return PromptStatus::Failed;
        return PromptStatus::Pending;
    }

    std::wstring ToString(PromptGate g)
    {
        switch (g)
        {
        case PromptGate::AfterDelay:
            return L"AfterDelay";
        case PromptGate::Manual:
            return L"Manual";
        case PromptGate::OnTurnComplete:
        default:
            return L"OnTurnComplete";
        }
    }
    PromptGate PromptGateFromString(std::wstring_view s)
    {
        if (s == L"AfterDelay")
            return PromptGate::AfterDelay;
        if (s == L"Manual")
            return PromptGate::Manual;
        return PromptGate::OnTurnComplete;
    }

    std::wstring ToString(PromptOrigin o)
    {
        // "Autorun" (was "Flight" before the Auto Testing rename); PromptOriginFromString maps any
        // non-"Typed" value — including a pre-rename "Flight" — back to Autorun, so old files still read.
        return o == PromptOrigin::Typed ? L"Typed" : L"Autorun";
    }
    PromptOrigin PromptOriginFromString(std::wstring_view s)
    {
        return s == L"Typed" ? PromptOrigin::Typed : PromptOrigin::Autorun;
    }

    // ---- struct <-> json ----

    json::Value ToJson(const QueuedPrompt& p)
    {
        auto o = json::Value::MkObj();
        o.Set(L"id", json::Value::MkStr(p.id));
        o.Set(L"label", json::Value::MkStr(p.label));
        o.Set(L"text", json::Value::MkStr(p.text));
        o.Set(L"status", json::Value::MkStr(ToString(p.status)));
        o.Set(L"gate", json::Value::MkStr(ToString(p.gate)));
        o.Set(L"delayMs", json::Value::MkNum(p.delayMs));
        o.Set(L"guardPattern", json::Value::MkStr(p.guardPattern));
        if (p.dependsOn)
        {
            o.Set(L"dependsOn", json::Value::MkStr(*p.dependsOn));
        }
        o.Set(L"attempts", json::Value::MkNum(p.attempts));
        o.Set(L"maxAttempts", json::Value::MkNum(p.maxAttempts));
        o.Set(L"sentAtUnixMs", json::Value::MkNum(static_cast<double>(p.sentAtUnixMs)));
        o.Set(L"origin", json::Value::MkStr(ToString(p.origin)));
        return o;
    }

    QueuedPrompt PromptFromJson(const json::Value& v)
    {
        QueuedPrompt p;
        p.id = v.StrAt(L"id");
        p.label = v.StrAt(L"label");
        p.text = v.StrAt(L"text");
        p.status = PromptStatusFromString(v.StrAt(L"status", L"Pending"));
        p.gate = PromptGateFromString(v.StrAt(L"gate", L"OnTurnComplete"));
        p.delayMs = v.U32At(L"delayMs");
        p.guardPattern = v.StrAt(L"guardPattern");
        if (const auto* d = v.Find(L"dependsOn"); d && d->type == json::Value::Type::Str)
        {
            p.dependsOn = d->str;
        }
        p.attempts = v.U32At(L"attempts");
        p.maxAttempts = v.U32At(L"maxAttempts", 1);
        p.sentAtUnixMs = v.I64At(L"sentAtUnixMs");
        p.origin = PromptOriginFromString(v.StrAt(L"origin", L"Autorun"));
        // `echoed` is transient (not persisted): a reloaded Sent prompt's echo already
        // happened in a past run; the recency window stops it from matching a fresh message.
        return p;
    }

    json::Value ToJson(const AutorunnerState& a)
    {
        auto o = json::Value::MkObj();
        o.Set(L"mode", json::Value::MkStr(ToString(a.mode)));
        o.Set(L"throttleMs", json::Value::MkNum(a.throttleMs));
        o.Set(L"stopOnError", json::Value::MkBool(a.stopOnError));
        o.Set(L"pauseOnHumanInput", json::Value::MkBool(a.pauseOnHumanInput));
        o.Set(L"maxAutoSends", json::Value::MkNum(a.maxAutoSends));
        auto approval = json::Value::MkObj();
        approval.Set(L"pauseForHuman", json::Value::MkBool(a.approval.pauseForHuman));
        auto tools = json::Value::MkArr();
        for (const auto& t : a.approval.autoApproveTools)
        {
            tools.Push(json::Value::MkStr(t));
        }
        approval.Set(L"autoApproveTools", std::move(tools));
        o.Set(L"approval", std::move(approval));
        return o;
    }

    AutorunnerState AutorunnerFromJson(const json::Value& v)
    {
        AutorunnerState a;
        a.mode = AutorunnerModeFromString(v.StrAt(L"mode", L"Off"));
        a.throttleMs = v.U32At(L"throttleMs", 500);
        a.stopOnError = v.BoolAt(L"stopOnError", true);
        a.pauseOnHumanInput = v.BoolAt(L"pauseOnHumanInput", true);
        a.maxAutoSends = v.U32At(L"maxAutoSends", 100);
        if (const auto* ap = v.Find(L"approval"); ap && ap->type == json::Value::Type::Obj)
        {
            a.approval.pauseForHuman = ap->BoolAt(L"pauseForHuman", true);
            if (const auto* tools = ap->Find(L"autoApproveTools"); tools && tools->type == json::Value::Type::Arr)
            {
                for (const auto& t : tools->arr)
                {
                    a.approval.autoApproveTools.push_back(t.AsStr());
                }
            }
        }
        return a;
    }

    json::Value ToJson(const SessionInfo& s)
    {
        auto o = json::Value::MkObj();
        o.Set(L"id", json::Value::MkStr(s.id));
        o.Set(L"title", json::Value::MkStr(s.title));
        o.Set(L"workingDir", json::Value::MkStr(s.workingDir));
        o.Set(L"branch", json::Value::MkStr(s.branch));
        // Agentmaster (tab color modes): the session's OWN color (Individual mode) + the inferred
        // working dir (InferredWorkingDirectory mode) — both PERSISTED so a reopened session wears
        // the same color immediately (per-session permanence / no cwd->inferred color flip). Both
        // omitted when empty (the default + every pre-feature record), so an untouched
        // sessions.json is byte-unchanged.
        if (!s.tabColorHex.empty())
        {
            o.Set(L"tabColorHex", json::Value::MkStr(s.tabColorHex));
        }
        if (!s.inferredWorkingDir.empty())
        {
            o.Set(L"inferredWorkingDir", json::Value::MkStr(s.inferredWorkingDir));
        }
        o.Set(L"state", json::Value::MkStr(ToString(s.state)));
        o.Set(L"lastActivityUnixMs", json::Value::MkNum(static_cast<double>(s.lastActivityUnixMs)));
        o.Set(L"external", json::Value::MkBool(s.external));
        // Agentmaster (crash/restore fidelity — Rule #16): persist the question-guard flag so a crash
        // while the agent was WaitingForInput on a clarifying question can't drop the guard on reopen
        // (else a queued prompt would auto-ANSWER the question). Omitted when false (the common case), so
        // an all-normal sessions.json is byte-unchanged; restored only alongside a preserved needs-you
        // state (RestoredQuestionFlag). See SessionInfo::lastMessageWasQuestion.
        if (s.lastMessageWasQuestion)
        {
            o.Set(L"lastMessageWasQuestion", json::Value::MkBool(true));
        }
        // Codex managed-session support: persist the agent kind + the rollout resume target. Both
        // are omitted when default (Claude / empty), so an all-Claude sessions.json is byte-unchanged.
        if (s.kind == AgentKind::Codex)
        {
            o.Set(L"kind", json::Value::MkStr(L"Codex"));
        }
        if (!s.codexSessionId.empty())
        {
            o.Set(L"codexSessionId", json::Value::MkStr(s.codexSessionId));
        }
        // Agentmaster (never-messaged fork restore — SessionModels.h SessionInfo::forkParentId): the
        // fork SOURCE id. A fork writes its OWN transcript only on its first turn, so a fork that was
        // never messaged has none — persisting the source lets _LaunchClaudeSession re-fork from it
        // (into the same id) on restore instead of minting a fresh empty conversation. Omitted when
        // empty (a non-fork, or a fork that already has its own conversation), so a sessions.json with
        // no pending forks is byte-unchanged.
        if (!s.forkParentId.empty())
        {
            o.Set(L"forkParentId", json::Value::MkStr(s.forkParentId));
        }
        auto q = json::Value::MkArr();
        for (const auto& p : s.queue)
        {
            q.Push(ToJson(p));
        }
        o.Set(L"queue", std::move(q));
        o.Set(L"autorunner", ToJson(s.autorunner));
        return o;
    }

    SessionInfo SessionFromJson(const json::Value& v)
    {
        SessionInfo s;
        s.id = v.StrAt(L"id");
        s.title = v.StrAt(L"title");
        s.workingDir = v.StrAt(L"workingDir");
        s.branch = v.StrAt(L"branch");
        s.tabColorHex = v.StrAt(L"tabColorHex"); // PERSISTED (tab color modes): the session's own color; absent => "" (none dealt)
        s.inferredWorkingDir = v.StrAt(L"inferredWorkingDir"); // PERSISTED (tab color modes): the inferred workdir cache; absent => "" (not inferred yet)
        s.state = SessionStateFromString(v.StrAt(L"state", L"Idle"));
        s.lastActivityUnixMs = v.I64At(L"lastActivityUnixMs");
        s.external = v.BoolAt(L"external", false);
        s.lastMessageWasQuestion = v.BoolAt(L"lastMessageWasQuestion", false); // PERSISTED (Rule #16): absent => false (back-compat + the common case)
        s.kind = (v.StrAt(L"kind", L"Claude") == L"Codex") ? AgentKind::Codex : AgentKind::Claude; // absent => Claude (back-compat)
        s.codexSessionId = v.StrAt(L"codexSessionId");
        s.forkParentId = v.StrAt(L"forkParentId"); // PERSISTED: the fork SOURCE, for re-forking a never-messaged fork on restore (absent => "")
        if (const auto* q = v.Find(L"queue"); q && q->type == json::Value::Type::Arr)
        {
            for (const auto& pv : q->arr)
            {
                s.queue.push_back(PromptFromJson(pv));
            }
        }
        // MIGRATION: the per-session block was renamed "autopilot" -> "autorunner" (Autopilot ->
        // Tests Autorunner). Read the old key when the new one is absent so a pre-rename sessions.json
        // keeps each session's mode + backstops.
        const auto* a = v.Find(L"autorunner");
        if (!a)
        {
            a = v.Find(L"autopilot");
        }
        if (a && a->type == json::Value::Type::Obj)
        {
            s.autorunner = AutorunnerFromJson(*a);
        }
        return s;
    }

    json::Value ToJson(const PlanTemplate& t)
    {
        auto o = json::Value::MkObj();
        o.Set(L"name", json::Value::MkStr(t.name));
        auto ps = json::Value::MkArr();
        for (const auto& p : t.prompts)
        {
            ps.Push(ToJson(p));
        }
        o.Set(L"prompts", std::move(ps));
        return o;
    }

    PlanTemplate TemplateFromJson(const json::Value& v)
    {
        PlanTemplate t;
        t.name = v.StrAt(L"name");
        if (const auto* ps = v.Find(L"prompts"); ps && ps->type == json::Value::Type::Arr)
        {
            for (const auto& pv : ps->arr)
            {
                t.prompts.push_back(PromptFromJson(pv));
            }
        }
        return t;
    }

    json::Value ToJson(const AppSettings& s)
    {
        auto o = json::Value::MkObj();
        o.Set(L"skipPermissions", json::Value::MkBool(s.skipPermissions));
        o.Set(L"model", json::Value::MkStr(s.model));
        o.Set(L"includeCoAuthoredBy", json::Value::MkBool(s.includeCoAuthoredBy));
        o.Set(L"env", json::Value::MkStr(s.env));
        o.Set(L"claudeExePath", json::Value::MkStr(s.claudeExePath));
        o.Set(L"defaultAutorunnerMode", json::Value::MkStr(ToString(s.defaultAutorunnerMode)));
        o.Set(L"maxAutoSends", json::Value::MkNum(s.maxAutoSends));
        o.Set(L"stopOnError", json::Value::MkBool(s.stopOnError));
        o.Set(L"pauseOnHumanInput", json::Value::MkBool(s.pauseOnHumanInput));
        o.Set(L"confirmBeforeKill", json::Value::MkBool(s.confirmBeforeKill));
        o.Set(L"tabRenameCommitMode", json::Value::MkStr(ToString(s.tabRenameCommitMode)));
        o.Set(L"defaultLaunchDir", json::Value::MkStr(s.defaultLaunchDir));
        o.Set(L"waitingForYouTimeoutMinutes", json::Value::MkNum(s.waitingForYouTimeoutMinutes));
        o.Set(L"serverCacheMinutes", json::Value::MkNum(s.serverCacheMinutes));
        o.Set(L"recentDirsLimit", json::Value::MkNum(s.recentDirsLimit));
        o.Set(L"maxTags", json::Value::MkNum(s.maxTags));
        o.Set(L"tooltipTagsOpacity", json::Value::MkNum(s.tooltipTagsOpacity));
        o.Set(L"showTabCloseButton", json::Value::MkBool(s.showTabCloseButton));
        o.Set(L"closeTabOnMiddleClick", json::Value::MkBool(s.closeTabOnMiddleClick));
        o.Set(L"alwaysShowHomeButton", json::Value::MkBool(s.alwaysShowHomeButton));
        o.Set(L"favoriteIcon", json::Value::MkStr(ToString(s.favoriteIcon)));
        o.Set(L"tabColorMode", json::Value::MkStr(ToString(s.tabColorMode)));
        o.Set(L"inferGitRoot", json::Value::MkBool(s.inferGitRoot));
        o.Set(L"flashRingColor", json::Value::MkStr(s.flashRingColor));
        o.Set(L"pendingDotsLightColor", json::Value::MkStr(s.pendingDotsLightColor));
        o.Set(L"pendingDotsDarkColor", json::Value::MkStr(s.pendingDotsDarkColor));
        o.Set(L"showTabOverlay", json::Value::MkBool(s.showTabOverlay));
        o.Set(L"tabOverlayRestOpacity", json::Value::MkNum(s.tabOverlayRestOpacity));
        o.Set(L"tabOverlayHoverOpacity", json::Value::MkNum(s.tabOverlayHoverOpacity));
        o.Set(L"showSummaryPanel", json::Value::MkBool(s.showSummaryPanel));
        o.Set(L"summaryPanelWrapNewlines", json::Value::MkBool(s.summaryPanelWrapNewlines));
        o.Set(L"summaryPanelTruncate", json::Value::MkBool(s.summaryPanelTruncate));
        o.Set(L"summaryPanelShowPrevious", json::Value::MkBool(s.summaryPanelShowPrevious));
        o.Set(L"treeSort", json::Value::MkStr(ToString(s.treeSort)));
        o.Set(L"boardSort", json::Value::MkStr(ToString(s.boardSort)));
        o.Set(L"autoTestingShowsSummary", json::Value::MkBool(s.autoTestingShowsSummary));
        o.Set(L"archiveSplitFraction", json::Value::MkNum(s.archiveSplitFraction));
        o.Set(L"summaryPanelWidthFraction", json::Value::MkNum(s.summaryPanelWidthFraction));
        o.Set(L"summaryPanelHeightFraction", json::Value::MkNum(s.summaryPanelHeightFraction));
        o.Set(L"allowUpdatePrerelease", json::Value::MkBool(s.allowUpdatePrerelease));
        o.Set(L"updateSkippedVersion", json::Value::MkStr(s.updateSkippedVersion));
        o.Set(L"updatePostponedUntilUnixMs", json::Value::MkNum(static_cast<double>(s.updatePostponedUntilUnixMs)));
        auto hidden = json::Value::MkArr();
        for (const auto& id : s.hiddenSessionIds)
        {
            hidden.Push(json::Value::MkStr(id));
        }
        o.Set(L"hiddenSessionIds", std::move(hidden));
        o.Set(L"envDefaultsVersion", json::Value::MkNum(s.envDefaultsVersion));
        o.Set(L"claudeCleanupDaysSeeded", json::Value::MkBool(s.claudeCleanupDaysSeeded));
        return o;
    }

    AppSettings AppSettingsFromJson(const json::Value& v)
    {
        AppSettings s; // any missing field keeps the struct default (== prior hardcoded behavior)
        s.skipPermissions = v.BoolAt(L"skipPermissions", true);
        s.model = v.StrAt(L"model");
        s.includeCoAuthoredBy = v.BoolAt(L"includeCoAuthoredBy", true);
        s.env = v.StrAt(L"env");
        s.claudeExePath = v.StrAt(L"claudeExePath");
        // Default Full (Agentmaster): a missing field => Full, matching the struct default so an
        // older / absent settings.json also opts every new/opened session into Autorunner.
        // MIGRATION: renamed from "defaultAutopilotMode" (Autopilot -> Tests Autorunner). Fall back
        // to the old key when the new one is absent so a pre-rename stored default survives.
        s.defaultAutorunnerMode = AutorunnerModeFromString(
            v.Find(L"defaultAutorunnerMode") ? v.StrAt(L"defaultAutorunnerMode", L"Full") :
                                               v.StrAt(L"defaultAutopilotMode", L"Full"));
        s.maxAutoSends = v.U32At(L"maxAutoSends", 100);
        s.stopOnError = v.BoolAt(L"stopOnError", true);
        s.pauseOnHumanInput = v.BoolAt(L"pauseOnHumanInput", true);
        s.confirmBeforeKill = v.BoolAt(L"confirmBeforeKill", true);
        s.tabRenameCommitMode = TabRenameCommitModeFromString(v.StrAt(L"tabRenameCommitMode", L"shiftEnter"));
        s.defaultLaunchDir = v.StrAt(L"defaultLaunchDir");
        // A STORED 0 is meaningful (= never decay) — U32At only falls back when the key is absent.
        // RENAMED from "waitingDecayMinutes" on purpose (the Waiting-for-you behavior changed): the
        // legacy key is intentionally NOT read, so a pre-existing settings.json falls back to the new
        // 4320 (3d) default instead of carrying over a value tuned for the old 5-minute cache window.
        s.waitingForYouTimeoutMinutes = v.U32At(L"waitingForYouTimeoutMinutes", 4320);
        s.serverCacheMinutes = v.U32At(L"serverCacheMinutes", 5);
        s.recentDirsLimit = v.U32At(L"recentDirsLimit", 10);
        // Bookmark-tag global cap: absent => 20; clamped to the cog's 1..40 band (a hand-edited
        // 0/garbage self-heals to the default, an over-ceiling value to 40).
        s.maxTags = ClampMaxTags(v.U32At(L"maxTags", 20));
        // Tooltip tag-chip opacity: absent => 0.9 (90% solid); clamped to the cog slider's 0.1..1.0 band.
        s.tooltipTagsOpacity = ClampTooltipTagsOpacity(v.NumAt(L"tooltipTagsOpacity", 0.9));
        s.showTabCloseButton = v.BoolAt(L"showTabCloseButton", true); // absent => ON (theme-driven, the prior behavior)
        s.closeTabOnMiddleClick = v.BoolAt(L"closeTabOnMiddleClick", true); // absent => ON (close on middle click, the prior behavior)
        s.alwaysShowHomeButton = v.BoolAt(L"alwaysShowHomeButton", true); // absent => ON (the Home button is always shown by default)
        s.favoriteIcon = FavoriteIconFromString(v.StrAt(L"favoriteIcon", L"crown")); // FAVORITES.md §5a: absent/unknown => Crown (the prior behavior)
        s.tabColorMode = TabColorModeFromString(v.StrAt(L"tabColorMode", L"workingDirectory")); // tab color modes: absent/unknown => shared-per-working-dir (the prior behavior)
        s.inferGitRoot = v.BoolAt(L"inferGitRoot", true); // "Use .git folder to infer": absent => ON (the inferred dir snaps to the enclosing git root)
        // Status-dot flash-ring color (with opacity in the alpha byte). Absent => "#CCFF0000" (red at
        // 80% opacity). Stored verbatim; the UI-layer parser (ParseArgbHexColor) falls back to that
        // default on a malformed value, so a hand-edited garbage string self-heals on next save.
        s.flashRingColor = v.StrAt(L"flashRingColor", L"#CCFF0000");
        // Unsent-draft "3 dots" contrast pair (PENDING_INPUT.md). Absent => the historical gold on dark
        // (#FFE0A92B) + a deep amber on light (#FF5A3E00). Stored verbatim; the UI-layer parser
        // (ParseArgbHexColor) falls back to these defaults on a malformed value, so a hand-edit self-heals.
        s.pendingDotsLightColor = v.StrAt(L"pendingDotsLightColor", L"#FFE0A92B");
        s.pendingDotsDarkColor = v.StrAt(L"pendingDotsDarkColor", L"#FF5A3E00");
        s.showTabOverlay = v.BoolAt(L"showTabOverlay", true);
        {
            // The per-tab overlay REST/HOVER opacities (TAB_OVERLAY.md): clamp each to (0, 1] and enforce
            // the rest <= hover invariant (the dual-thumb slider can't cross them; a hand-edit that does is
            // corrected here — keep hover, clamp rest down to it). A degenerate/absent value falls back to
            // the default look (rest 0.50, hover 1.0).
            double rest = v.NumAt(L"tabOverlayRestOpacity", 0.50);
            double hover = v.NumAt(L"tabOverlayHoverOpacity", 1.0);
            if (!(rest > 0.0 && rest <= 1.0))
            {
                rest = 0.50;
            }
            if (!(hover > 0.0 && hover <= 1.0))
            {
                hover = 1.0;
            }
            if (rest > hover)
            {
                rest = hover; // crossed -> clamp rest down to hover (preserve the hover intent)
            }
            s.tabOverlayRestOpacity = rest;
            s.tabOverlayHoverOpacity = hover;
        }
        s.showSummaryPanel = v.BoolAt(L"showSummaryPanel", true); // TAB_OVERLAY.md summary panel toggle (absent => ON by default)
        s.summaryPanelWrapNewlines = v.BoolAt(L"summaryPanelWrapNewlines", false); // TAB_OVERLAY.md: preserve message newlines (absent => OFF, the literal-\n look)
        s.summaryPanelTruncate = v.BoolAt(L"summaryPanelTruncate", true); // TAB_OVERLAY.md: truncate long messages (absent => ON by default, cap each message)
        s.summaryPanelShowPrevious = v.BoolAt(L"summaryPanelShowPrevious", false); // conversation lineage: show pre-compaction previous session(s) (absent => OFF)
        s.treeSort = ExplorerSortFromString(v.StrAt(L"treeSort", L"newest"));
        // Triage Board sort (a separate global from treeSort). Absent => the board's MostActive default
        // (most-recently-active first). A stored "pid" would deserialize fine but the board never
        // produces it (its cycle skips ByPid), so it can only arrive via a hand-edit.
        s.boardSort = ExplorerSortFromString(v.StrAt(L"boardSort", L"active"));
        // Manager Auto-Testing pane tab (Agentmaster): absent => Summary (true), the default tab.
        // MIGRATION: renamed from "flightPlanShowsSummary" (Flight Plan -> Auto Testing). Fall back
        // to the old key when the new one is absent so a pre-rename choice survives.
        s.autoTestingShowsSummary = v.Find(L"autoTestingShowsSummary") ?
                                        v.BoolAt(L"autoTestingShowsSummary", true) :
                                        v.BoolAt(L"flightPlanShowsSummary", true);
        {
            // Same sane-band clamp as the Manager layout fractions — a corrupt/extreme value
            // must not collapse a pane (fall back to the 50/50 default instead).
            const double f = v.NumAt(L"archiveSplitFraction", 0.5);
            s.archiveSplitFraction = (f > 0.05 && f < 0.95) ? f : 0.5;
        }
        {
            // Summary panel size fractions (TAB_OVERLAY.md): 0 == "auto" (the original look). A stored
            // explicit fraction must be within the same band the resize grips clamp to — width
            // (0.08, 0.5], height (0.06, 0.75] — else fall back to 0 (auto), so a corrupt value can't
            // wedge the panel at a degenerate size.
            const double wf = v.NumAt(L"summaryPanelWidthFraction", 0.0);
            s.summaryPanelWidthFraction = (wf >= 0.08 && wf <= 0.5) ? wf : 0.0;
            const double hf = v.NumAt(L"summaryPanelHeightFraction", 0.0);
            s.summaryPanelHeightFraction = (hf >= 0.06 && hf <= 0.75) ? hf : 0.0;
        }
        // Updater (Updater.h): the prerelease opt-in + the skip/postpone state (the latter two are
        // written OUTSIDE the cog form by the updater's JSON RMW; the cog's Save preserves them).
        s.allowUpdatePrerelease = v.BoolAt(L"allowUpdatePrerelease", false);
        s.updateSkippedVersion = v.StrAt(L"updateSkippedVersion");
        s.updatePostponedUntilUnixMs = v.I64At(L"updatePostponedUntilUnixMs", 0);
        if (const auto* h = v.Find(L"hiddenSessionIds"); h && h->type == json::Value::Type::Arr)
        {
            for (const auto& e : h->arr)
            {
                if (e.type == json::Value::Type::Str && !e.str.empty())
                {
                    s.hiddenSessionIds.push_back(e.str);
                }
            }
        }
        // Shipped-default seeding markers (ENV_VARS.md §8). Absent => 0 / false, so a pre-feature
        // settings.json runs the one-time seed once (new installs + updaters alike get the defaults).
        s.envDefaultsVersion = v.U32At(L"envDefaultsVersion", 0);
        s.claudeCleanupDaysSeeded = v.BoolAt(L"claudeCleanupDaysSeeded", false);
        return s;
    }

    // ---- workspace persistence: the per-window record + its parts (M10) ----

    json::Value ToJson(const ManagerLayout& l)
    {
        auto o = json::Value::MkObj();
        o.Set(L"boardFraction", json::Value::MkNum(l.boardFraction));
        o.Set(L"treeFraction", json::Value::MkNum(l.treeFraction));
        return o;
    }

    ManagerLayout ManagerLayoutFromJson(const json::Value& v)
    {
        ManagerLayout l;
        // Same sane-band clamp as DeserializeLayout, so a corrupt fraction can't collapse a pane.
        auto sane = [](double x, double fb) { return (x > 0.05 && x < 0.95) ? x : fb; };
        l.boardFraction = sane(v.NumAt(L"boardFraction", l.boardFraction), l.boardFraction);
        l.treeFraction = sane(v.NumAt(L"treeFraction", l.treeFraction), l.treeFraction);
        return l;
    }

    json::Value ToJson(const WindowGeometry& g)
    {
        auto o = json::Value::MkObj();
        o.Set(L"hasPosition", json::Value::MkBool(g.hasPosition));
        o.Set(L"x", json::Value::MkNum(g.x));
        o.Set(L"y", json::Value::MkNum(g.y));
        o.Set(L"hasSize", json::Value::MkBool(g.hasSize));
        o.Set(L"width", json::Value::MkNum(g.width));
        o.Set(L"height", json::Value::MkNum(g.height));
        o.Set(L"launchMode", json::Value::MkStr(g.launchMode));
        return o;
    }

    WindowGeometry GeometryFromJson(const json::Value& v)
    {
        WindowGeometry g;
        g.hasPosition = v.BoolAt(L"hasPosition", false);
        g.x = v.NumAt(L"x", 0);
        g.y = v.NumAt(L"y", 0);
        g.hasSize = v.BoolAt(L"hasSize", false);
        g.width = v.NumAt(L"width", 0);
        g.height = v.NumAt(L"height", 0);
        g.launchMode = v.StrAt(L"launchMode");
        return g;
    }

    json::Value ToJson(const TabEntry& t)
    {
        auto o = json::Value::MkObj();
        o.Set(L"kind", json::Value::MkStr(t.kind == TabKind::Other ? L"Other" : (t.kind == TabKind::Codex ? L"Codex" : L"Claude")));
        if (t.kind == TabKind::Claude || t.kind == TabKind::Codex)
        {
            o.Set(L"sessionId", json::Value::MkStr(t.sessionId)); // a reference; the record (incl. the Codex resume uuid) lives in sessions.json
        }
        else
        {
            o.Set(L"actionsJson", json::Value::MkStr(t.actionsJson));
        }
        if (!t.tabColor.empty())
        {
            o.Set(L"tabColor", json::Value::MkStr(t.tabColor));
        }
        return o;
    }

    TabEntry TabEntryFromJson(const json::Value& v)
    {
        TabEntry t;
        const auto tk = v.StrAt(L"kind", L"Claude");
        t.kind = (tk == L"Other") ? TabKind::Other : (tk == L"Codex" ? TabKind::Codex : TabKind::Claude);
        if (t.kind == TabKind::Claude || t.kind == TabKind::Codex)
        {
            t.sessionId = v.StrAt(L"sessionId");
        }
        else
        {
            t.actionsJson = v.StrAt(L"actionsJson");
        }
        t.tabColor = v.StrAt(L"tabColor");
        return t;
    }

    json::Value ToJson(const ManagerState& m)
    {
        auto o = json::Value::MkObj();
        o.Set(L"selectedId", json::Value::MkStr(m.selectedId));
        o.Set(L"scopeDir", json::Value::MkStr(m.scopeDir));
        o.Set(L"selectedPromptId", json::Value::MkStr(m.selectedPromptId));
        auto cd = json::Value::MkArr();
        for (const auto& d : m.collapsedDirs)
        {
            cd.Push(json::Value::MkStr(d));
        }
        o.Set(L"collapsedDirs", std::move(cd));
        o.Set(L"layout", ToJson(m.layout));
        o.Set(L"treeScope", json::Value::MkNum(static_cast<double>(m.treeScope)));
        return o;
    }

    ManagerState ManagerStateFromJson(const json::Value& v)
    {
        ManagerState m;
        m.selectedId = v.StrAt(L"selectedId");
        m.scopeDir = v.StrAt(L"scopeDir");
        m.selectedPromptId = v.StrAt(L"selectedPromptId");
        // The shared tree/board scope (0=LOCAL 1=GLOBAL 2=EXTERNAL); clamp an out-of-range value
        // (a hand-edited record) back to LOCAL rather than indexing a nonexistent mode.
        {
            const int scope = static_cast<int>(v.NumAt(L"treeScope", 0.0));
            m.treeScope = (scope >= 0 && scope <= 2) ? scope : 0;
        }
        if (const auto* cd = v.Find(L"collapsedDirs"); cd && cd->type == json::Value::Type::Arr)
        {
            for (const auto& dv : cd->arr)
            {
                if (dv.type == json::Value::Type::Str)
                {
                    m.collapsedDirs.push_back(dv.AsStr());
                }
            }
        }
        if (const auto* l = v.Find(L"layout"); l && l->type == json::Value::Type::Obj)
        {
            m.layout = ManagerLayoutFromJson(*l);
        }
        return m;
    }

    json::Value ToJson(const WindowRecord& w)
    {
        auto o = json::Value::MkObj();
        o.Set(L"windowId", json::Value::MkStr(w.windowId));
        o.Set(L"geometry", ToJson(w.geometry));
        auto tabs = json::Value::MkArr();
        for (const auto& t : w.tabs)
        {
            tabs.Push(ToJson(t));
        }
        o.Set(L"tabs", std::move(tabs));
        o.Set(L"selectedSessionId", json::Value::MkStr(w.selectedSessionId)); // stable id of the focused Claude tab
        o.Set(L"selectedTabIndex", json::Value::MkNum(static_cast<double>(w.selectedTabIndex))); // fallback for a shell tab
        o.Set(L"manager", ToJson(w.manager));
        o.Set(L"managerTabColor", json::Value::MkStr(w.managerTabColor)); // Agentmaster: the Manager tab's per-window color ("#RRGGBB"; "" => none)
        return o;
    }

    WindowRecord WindowRecordFromJson(const json::Value& v)
    {
        WindowRecord w;
        w.windowId = v.StrAt(L"windowId");
        if (const auto* g = v.Find(L"geometry"); g && g->type == json::Value::Type::Obj)
        {
            w.geometry = GeometryFromJson(*g);
        }
        if (const auto* tabs = v.Find(L"tabs"); tabs && tabs->type == json::Value::Type::Arr)
        {
            for (const auto& tv : tabs->arr)
            {
                w.tabs.push_back(TabEntryFromJson(tv));
            }
        }
        // Both unset (empty / -1) when absent — a record from before selected-tab persistence reopens
        // with the Manager tab focused, exactly as it did then. selectedSessionId (stable Claude id) is
        // preferred on restore; selectedTabIndex is the shell-tab fallback.
        w.selectedSessionId = v.StrAt(L"selectedSessionId");
        w.selectedTabIndex = static_cast<int>(v.I64At(L"selectedTabIndex", -1));
        w.managerTabColor = v.StrAt(L"managerTabColor"); // empty when absent (a pre-feature record => no Manager-tab color)
        if (const auto* m = v.Find(L"manager"); m && m->type == json::Value::Type::Obj)
        {
            w.manager = ManagerStateFromJson(*m);
        }
        return w;
    }

    // ---- whole document ----

    std::wstring SerializeSessions(const std::vector<SessionInfo>& sessions)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        auto arr = json::Value::MkArr();
        for (const auto& s : sessions)
        {
            arr.Push(ToJson(s));
        }
        root.Set(L"sessions", std::move(arr));
        return json::Dump(root);
    }

    std::vector<SessionInfo> DeserializeSessions(std::wstring_view text)
    {
        std::vector<SessionInfo> out;
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return out;
        }
        if (const auto* arr = parsed->Find(L"sessions"); arr && arr->type == json::Value::Type::Arr)
        {
            for (const auto& sv : arr->arr)
            {
                out.push_back(SessionFromJson(sv));
            }
        }
        return out;
    }

    std::wstring SerializeTemplates(const std::vector<PlanTemplate>& templates)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        auto arr = json::Value::MkArr();
        for (const auto& t : templates)
        {
            arr.Push(ToJson(t));
        }
        root.Set(L"templates", std::move(arr));
        return json::Dump(root);
    }

    std::vector<PlanTemplate> DeserializeTemplates(std::wstring_view text)
    {
        std::vector<PlanTemplate> out;
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return out;
        }
        if (const auto* arr = parsed->Find(L"templates"); arr && arr->type == json::Value::Type::Arr)
        {
            for (const auto& tv : arr->arr)
            {
                out.push_back(TemplateFromJson(tv));
            }
        }
        return out;
    }

    std::wstring SerializeRecentDirs(const std::vector<std::wstring>& dirs)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        auto arr = json::Value::MkArr();
        for (const auto& d : dirs)
        {
            arr.Push(json::Value::MkStr(d));
        }
        root.Set(L"dirs", std::move(arr));
        return json::Dump(root);
    }

    std::vector<std::wstring> DeserializeRecentDirs(std::wstring_view text)
    {
        std::vector<std::wstring> out;
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return out;
        }
        if (const auto* arr = parsed->Find(L"dirs"); arr && arr->type == json::Value::Type::Arr)
        {
            for (const auto& dv : arr->arr)
            {
                if (dv.type == json::Value::Type::Str)
                {
                    out.push_back(dv.AsStr());
                }
            }
        }
        return out;
    }

    std::wstring SerializeOpenWindows(const std::vector<std::wstring>& windowIds)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        auto arr = json::Value::MkArr();
        for (const auto& id : windowIds)
        {
            arr.Push(json::Value::MkStr(id));
        }
        root.Set(L"open", std::move(arr));
        return json::Dump(root);
    }

    std::vector<std::wstring> DeserializeOpenWindows(std::wstring_view text)
    {
        std::vector<std::wstring> out;
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return out;
        }
        if (const auto* arr = parsed->Find(L"open"); arr && arr->type == json::Value::Type::Arr)
        {
            for (const auto& dv : arr->arr)
            {
                if (dv.type == json::Value::Type::Str)
                {
                    out.push_back(dv.AsStr());
                }
            }
        }
        return out;
    }

    std::wstring SerializeLayout(const ManagerLayout& layout)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        root.Set(L"boardFraction", json::Value::MkNum(layout.boardFraction));
        root.Set(L"treeFraction", json::Value::MkNum(layout.treeFraction));
        return json::Dump(root);
    }

    ManagerLayout DeserializeLayout(std::wstring_view text)
    {
        ManagerLayout layout; // defaults stand in for a missing/corrupt field
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return layout;
        }
        // Keep each fraction inside a sane band so a hand-edited/corrupt file can never
        // collapse a pane to nothing. The out-of-band test also rejects NaN.
        auto sane = [](double v, double fallback) {
            return (v > 0.05 && v < 0.95) ? v : fallback;
        };
        layout.boardFraction = sane(parsed->NumAt(L"boardFraction", layout.boardFraction), layout.boardFraction);
        layout.treeFraction = sane(parsed->NumAt(L"treeFraction", layout.treeFraction), layout.treeFraction);
        return layout;
    }

    std::wstring SerializeAppSettings(const AppSettings& settings)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        root.Set(L"settings", ToJson(settings));
        return json::Dump(root);
    }

    AppSettings DeserializeAppSettings(std::wstring_view text)
    {
        AppSettings s; // defaults stand in for a missing/corrupt file or field
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return s;
        }
        if (const auto* o = parsed->Find(L"settings"); o && o->type == json::Value::Type::Obj)
        {
            s = AppSettingsFromJson(*o);
        }
        return s;
    }

    std::wstring SerializeWindowRecord(const WindowRecord& record)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        root.Set(L"window", ToJson(record));
        return json::Dump(root);
    }

    WindowRecord DeserializeWindowRecord(std::wstring_view text)
    {
        WindowRecord w;
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return w;
        }
        if (const auto* o = parsed->Find(L"window"); o && o->type == json::Value::Type::Obj)
        {
            w = WindowRecordFromJson(*o);
        }
        return w;
    }

    // ---- disk ----

    void SaveSessions(const std::vector<SessionInfo>& sessions)
    {
        WriteAllUtf8(AgentmasterStateDir() + L"\\sessions.json", SerializeSessions(sessions));
    }
    std::vector<SessionInfo> LoadSessions()
    {
        return DeserializeSessions(ReadAllUtf8(AgentmasterStateDir() + L"\\sessions.json"));
    }
    void SaveTemplates(const std::vector<PlanTemplate>& templates)
    {
        WriteAllUtf8(AgentmasterStateDir() + L"\\templates.json", SerializeTemplates(templates));
    }
    std::vector<PlanTemplate> LoadTemplates()
    {
        return DeserializeTemplates(ReadAllUtf8(AgentmasterStateDir() + L"\\templates.json"));
    }
    void SaveRecentDirs(const std::vector<std::wstring>& dirs)
    {
        WriteAllUtf8(AgentmasterStateDir() + L"\\recent-dirs.json", SerializeRecentDirs(dirs));
    }
    std::vector<std::wstring> LoadRecentDirs()
    {
        return DeserializeRecentDirs(ReadAllUtf8(AgentmasterStateDir() + L"\\recent-dirs.json"));
    }
    void SaveOpenWindows(const std::vector<std::wstring>& windowIds)
    {
        WriteAllUtf8(AgentmasterStateDir() + L"\\open-windows.json", SerializeOpenWindows(windowIds));
    }
    std::vector<std::wstring> LoadOpenWindows()
    {
        return DeserializeOpenWindows(ReadAllUtf8(AgentmasterStateDir() + L"\\open-windows.json"));
    }

    // ===== Tab naming + per-directory color (Agentmaster) =====

    namespace
    {
        std::wstring LowerCopy(std::wstring s)
        {
            for (auto& c : s)
            {
                c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
            }
            return s;
        }

        // Strip trailing path separators ('/' or '\\') but keep a bare root ("C:\\" / "/").
        std::wstring StripTrailingSep(std::wstring s)
        {
            while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/'))
            {
                if (s.size() == 3 && s[1] == L':')
                {
                    break; // "C:\" — keep the root separator
                }
                s.pop_back();
            }
            return s;
        }

        // dir-colors.json is read-modify-written; one process (M9) but many window threads.
        std::mutex g_dirColorMtx;
        // dir-env.json (the per-directory env overrides) is read-modify-written the same way.
        std::mutex g_dirEnvMtx;

        // --- Auto tab-color allocator (Agentmaster) -------------------------------------------------
        // A fixed palette of distinct, readable tab colors. A directory with no explicit (user-picked)
        // color is dealt one of these AUTO and the choice is **persisted to dir-colors.json**, so a
        // directory keeps the SAME color permanently — across its tabs, all windows, and restarts (the
        // user "gets used to" a folder's color). dir-colors.json is therefore the single source of
        // truth: the set of palette colors already assigned to some folder IS the "color collection".
        // A new dir is dealt the first color in its seeded probe order that no other folder already
        // holds; when every palette color is taken ("the color list is over") the collection resets —
        // colors may be reused, but the deal still avoids any color a tab is ACTIVELY showing in any
        // window. A user color pick overrides + persists the same way (and fans out to the dir's tabs).
        // g_dirColorMtx guards this state.
        const wchar_t* const kAutoPalette[] = {
            L"#E06C75", L"#E5C07B", L"#98C379", L"#56B6C2", L"#61AFEF", L"#C678DD",
            L"#D19A66", L"#BE5046", L"#528BFF", L"#7FD962", L"#FF9E64", L"#2BBAC5",
            L"#B267E6", L"#F78C6C",
        };
        constexpr size_t kAutoPaletteCount = sizeof(kAutoPalette) / sizeof(kAutoPalette[0]);

        // The seed only randomizes the probe ORDER for a dir's FIRST-ever assignment (which then
        // persists permanently); it never moves an already-assigned color. Engine init installs a
        // random one; a test/headless caller gets a lazy random one (or a fixed one via SeedDirColors).
        uint64_t g_colorSeed{ 0 };
        bool g_colorSeedSet{ false };

        // Install a fresh random seed if one was never set. The Engine seeds at init; this is the
        // fallback for headless/test callers that touch a color before the engine runs. Caller holds
        // g_dirColorMtx.
        void EnsureColorSeedLocked()
        {
            if (!g_colorSeedSet)
            {
                std::random_device rd;
                g_colorSeed = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
                g_colorSeedSet = true;
            }
        }

        // splitmix64 — a tiny, well-distributed PRNG step used to shuffle the probe order.
        uint64_t Splitmix64(uint64_t& s)
        {
            uint64_t z = (s += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        // The "fixed but randomized sequence": a deterministic permutation of palette indices for a
        // directory key, keyed by (seed, key). The first element is the dir's preferred color;
        // collision-avoidance walks the rest. Pure (the seed is passed in) so it is unit-testable.
        std::vector<size_t> ColorProbeOrder(const std::wstring& key, uint64_t seed)
        {
            uint64_t h = 1469598103934665603ull ^ seed; // FNV-1a basis, seeded
            for (const wchar_t c : key)
            {
                h ^= static_cast<uint64_t>(static_cast<uint16_t>(c));
                h *= 1099511628211ull;
            }
            std::vector<size_t> order(kAutoPaletteCount);
            for (size_t i = 0; i < kAutoPaletteCount; ++i)
            {
                order[i] = i;
            }
            uint64_t st = h ? h : 0x123456789abcdefull; // avoid an all-zero PRNG state
            for (size_t i = kAutoPaletteCount; i > 1; --i) // Fisher-Yates over the indices
            {
                const size_t j = static_cast<size_t>(Splitmix64(st) % i);
                std::swap(order[i - 1], order[j]);
            }
            return order;
        }

        // Case-insensitive "#RRGGBB" membership in the auto palette. The sound signal that a persisted
        // entry was AUTO-assigned (the auto path can only ever emit a palette color) vs an explicit
        // user pick (an off-palette hex) — used by ChooseDirColor (a user pick never blocks a palette
        // slot) and the one-time v1->v2 de-collide migration.
        bool IsAutoPaletteColor(const std::wstring& hex)
        {
            const std::wstring low = LowerCopy(hex);
            for (const auto* p : kAutoPalette)
            {
                if (low == LowerCopy(p))
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool IsGenericDirName(const std::wstring& segment)
    {
        // Top-20 generic build/output/dependency/structural folder names (compared case-folded).
        static const std::unordered_set<std::wstring> kGeneric = {
            L"bin", L"obj", L"debug", L"release", L"build", L"out", L"dist", L"target",
            L"publish", L"bld", L"x64", L"x86", L"win32", L"arm64", L"node_modules",
            L"packages", L"src", L"lib", L"temp", L"tmp",
        };
        return kGeneric.find(LowerCopy(segment)) != kGeneric.end();
    }

    std::wstring DeriveSessionTitle(const std::wstring& workingDir)
    {
        std::wstring base;
        try
        {
            // Walk up from the leaf to the first non-generic segment (K:\proj\bin\Debug -> "proj").
            std::filesystem::path cur{ StripTrailingSep(workingDir) };
            while (!cur.empty())
            {
                const std::wstring leaf = cur.filename().wstring();
                if (!leaf.empty() && leaf != L"." && leaf != L".." && !IsGenericDirName(leaf))
                {
                    base = leaf;
                    break;
                }
                const auto parent = cur.parent_path();
                if (parent.empty() || parent == cur)
                {
                    break; // reached the root with nothing but generic segments
                }
                cur = parent;
            }
            if (base.empty())
            {
                // All-generic (or rootless) -> fall back to the actual leaf.
                base = std::filesystem::path{ StripTrailingSep(workingDir) }.filename().wstring();
            }
        }
        catch (...)
        {
        }
        if (base.empty())
        {
            base = L"claude";
        }

        // Display rules by length/case.
        if (base.size() <= 16)
        {
            return base; // short -> as-is
        }
        bool allLower = true;
        for (const wchar_t c : base)
        {
            if (std::iswupper(static_cast<wint_t>(c)))
            {
                allLower = false;
                break;
            }
        }
        if (!allLower)
        {
            // >16 and has capitals -> the capital letters only ("MyLongProjectName" -> "MLPN").
            std::wstring caps;
            for (const wchar_t c : base)
            {
                if (std::iswupper(static_cast<wint_t>(c)))
                {
                    caps.push_back(c);
                }
            }
            if (!caps.empty())
            {
                return caps;
            }
        }
        // >16 all-lowercase -> as-is, truncated past 30 chars with an ellipsis.
        if (base.size() > 30)
        {
            return base.substr(0, 30) + L"...";
        }
        return base;
    }

    std::wstring DeriveForkTitle(const std::wstring& sourceTitle)
    {
        // Only a trailing " (fork)" or " (fork N)" group counts — recognize it and BUMP the counter
        // rather than appending another suffix (the "X (fork) (fork)" growth). A nested/earlier paren
        // group ("Foo (bar)") or a non-fork trailer ("Foo (1.0)") is left intact and just gets " (fork)".
        if (!sourceTitle.empty() && sourceTitle.back() == L')')
        {
            const auto open = sourceTitle.rfind(L'('); // the LAST '(' -> the trailing group, nested-paren safe
            if (open != std::wstring::npos && open > 0 && sourceTitle[open - 1] == L' ')
            {
                const std::wstring prefix = sourceTitle.substr(0, open - 1); // text before the " (" separator
                const std::wstring inner = sourceTitle.substr(open + 1, sourceTitle.size() - open - 2); // between ( and )
                if (inner == L"fork")
                {
                    return prefix + L" (fork 2)"; // the unnumbered first fork -> the second
                }
                if (inner.rfind(L"fork ", 0) == 0)
                {
                    const std::wstring numStr = inner.substr(5);
                    bool allDigits = !numStr.empty();
                    for (const wchar_t c : numStr)
                    {
                        if (c < L'0' || c > L'9')
                        {
                            allDigits = false;
                            break;
                        }
                    }
                    if (allDigits)
                    {
                        unsigned long long n = 0;
                        for (const wchar_t c : numStr)
                        {
                            n = n * 10ull + static_cast<unsigned long long>(c - L'0');
                        }
                        return prefix + L" (fork " + std::to_wstring(n + 1) + L")";
                    }
                }
            }
        }
        return sourceTitle + L" (fork)";
    }

    std::wstring NormDirKey(const std::wstring& dir)
    {
        std::wstring s = dir;
#ifdef _WIN32
        for (auto& ch : s)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
        }
        while (s.size() > 3 && s.back() == L'\\')
        {
            s.pop_back();
        }
        s = LowerCopy(s);
#else
        while (s.size() > 1 && s.back() == L'/')
        {
            s.pop_back();
        }
#endif
        return s;
    }

    void SeedDirColors(uint64_t seed)
    {
        std::lock_guard guard{ g_dirColorMtx };
        g_colorSeed = seed;
        g_colorSeedSet = true;
    }

    std::wstring AutoDirColorHex(const std::wstring& dir)
    {
        // PURE PREVIEW (no disk, no allocation): a representative color for a directory that has NO
        // persisted color yet — the first color of its seeded probe order. Callers that want the dir's
        // REAL (permanent) color check GetDirColor first (the Sessions-page chip does); this is only
        // the fallback for a never-assigned dir. The permanent, collision-free pick is AssignDirAutoColor.
        const std::wstring key = NormDirKey(dir);
        std::lock_guard guard{ g_dirColorMtx };
        EnsureColorSeedLocked();
        return kAutoPalette[ColorProbeOrder(key, g_colorSeed).front()];
    }

    std::wstring ChooseDirColor(const std::wstring& dirKey,
                                const std::vector<std::pair<std::wstring, std::wstring>>& existing,
                                const std::unordered_set<std::wstring>& activeColors,
                                uint64_t seed)
    {
        // PURE core of the permanent allocator (no disk; unit-testable). Given the existing folder ->
        // color map (dir-colors.json) and the colors currently shown by OPEN tabs, choose a color for
        // dirKey:
        //   0) already assigned -> return it (permanence: a folder never changes color on its own).
        //   1) the first palette color in the dir's probe order that NO folder already holds (keeps the
        //      folder<->color mapping unique while colors remain) — the normal case.
        //   2) "the color list is over" (every palette color already assigned to some folder): the
        //      collection RESETS — reuse is allowed, but still avoid any color an open tab is showing.
        //   3) every palette color is active (more open dirs than the 14 colors) — reuse is
        //      unavoidable; fall back to the dir's preferred color.
        for (const auto& [k, h] : existing)
        {
            if (k == dirKey)
            {
                return h;
            }
        }
        std::unordered_set<std::wstring> assigned; // palette colors already held by some folder
        for (const auto& [k, h] : existing)
        {
            if (IsAutoPaletteColor(h))
            {
                assigned.insert(h);
            }
        }
        const auto order = ColorProbeOrder(dirKey, seed);
        for (const size_t idx : order) // 1) a color no folder has yet
        {
            const std::wstring c = kAutoPalette[idx];
            if (assigned.find(c) == assigned.end())
            {
                return c;
            }
        }
        for (const size_t idx : order) // 2) palette exhausted -> reset; avoid actively-shown colors
        {
            const std::wstring c = kAutoPalette[idx];
            if (activeColors.find(c) == activeColors.end())
            {
                return c;
            }
        }
        return kAutoPalette[order.front()]; // 3) all colors active -> unavoidable reuse
    }

    std::wstring AssignDirAutoColor(const std::wstring& dir, const std::vector<std::wstring>& openDirKeys)
    {
        // Deal `dir` a PERMANENT auto color (ChooseDirColor) and persist it to dir-colors.json, so the
        // folder keeps that color across tabs / windows / restarts (Rule #12). Called only for a dir
        // with no persisted color yet (the caller checks GetDirColor first). User picks do NOT come
        // here — they flow through SetDirColor. Load-modify-save under one lock (LoadDirColors /
        // SaveDirColors don't lock). `openDirKeys` are the NormDirKeys of the currently-open dirs
        // (process-wide registry live set — so "actively shown" spans all windows).
        const std::wstring key = NormDirKey(dir);
        std::lock_guard guard{ g_dirColorMtx };
        EnsureColorSeedLocked();

        auto colors = LoadDirColors();
        const std::unordered_set<std::wstring> openSet(openDirKeys.begin(), openDirKeys.end());
        std::unordered_set<std::wstring> activeColors; // colors OTHER open dirs are currently showing
        for (const auto& [k, h] : colors)
        {
            if (k != key && openSet.find(k) != openSet.end())
            {
                activeColors.insert(h);
            }
        }

        const std::wstring chosen = ChooseDirColor(key, colors, activeColors, g_colorSeed);

        bool found = false; // upsert (chosen == the dir's existing entry if it already had one)
        for (auto& e : colors)
        {
            if (e.first == key)
            {
                e.second = chosen;
                found = true;
                break;
            }
        }
        if (!found)
        {
            colors.emplace_back(key, chosen);
        }
        SaveDirColors(colors); // permanent
        return chosen;
    }

    std::wstring ChooseSessionAutoColor(const std::wstring& sessionId,
                                        const std::vector<std::pair<std::wstring, std::wstring>>& liveSessionColors,
                                        const std::unordered_set<std::wstring>& activeColors)
    {
        // Tab color modes (Individual): the per-SESSION deal — ChooseDirColor's collision-avoiding
        // walk, keyed by the session id over the LIVE sessions' colors instead of the folder map.
        // Deliberately NO disk write and NO dir-colors.json involvement: the pick persists on the
        // SessionInfo record (the caller's registry Update -> sessions.json autosave), so Individual
        // mode never claims folder-palette slots (a mode switch back to per-dir coloring finds the
        // folder map exactly as it left it). The lock is only for the shared color seed.
        std::lock_guard guard{ g_dirColorMtx };
        EnsureColorSeedLocked();
        return ChooseDirColor(sessionId, liveSessionColors, activeColors, g_colorSeed);
    }

    std::wstring SessionColorKeyDir(TabColorMode mode, const SessionInfo& s)
    {
        // The dir that KEYS a session's color under `mode` (grouping + user-pick fan-out). Only
        // InferredWorkingDirectory diverges — and only once an inference EXISTS (until then the
        // launch cwd keys it, so a fresh session behaves exactly like WorkingDirectory mode).
        // Individual mode returns the working dir too: its callers branch on the mode BEFORE any
        // dir grouping (there is no dir key for per-session colors).
        if (mode == TabColorMode::InferredWorkingDirectory && !s.inferredWorkingDir.empty())
        {
            return s.inferredWorkingDir;
        }
        return s.workingDir;
    }

    std::wstring ResolveSessionColorHex(TabColorMode mode, const SessionInfo& s)
    {
        // The ONE read-side resolution of "what color does this session's tab wear" — shared by the
        // board title band / Sessions-page chip / pending-dots contrast so every surface matches the
        // tab. Individual => the session's own persisted color; a session with none dealt yet (or a
        // pre-feature/archived record) falls through to the dir-keyed precedence, matching the tab
        // until its first Individual paint deals one. Dir modes => persisted dir color, else the
        // deterministic AutoDirColorHex preview (the existing board/chip precedence).
        if (mode == TabColorMode::Individual && !s.tabColorHex.empty())
        {
            return s.tabColorHex;
        }
        const std::wstring keyDir = SessionColorKeyDir(mode, s);
        const auto persisted = GetDirColor(keyDir);
        return persisted ? *persisted : AutoDirColorHex(keyDir);
    }

    std::vector<std::pair<std::wstring, std::wstring>>
    DeCollideDirColors(const std::vector<std::pair<std::wstring, std::wstring>>& entries, uint64_t seed)
    {
        // PURE: make every PALETTE color unique across folders while KEEPING each folder's color where
        // possible (permanence). User picks (off-palette hexes) are kept verbatim and never block a
        // slot; for palette colors, the FIRST folder to use a color keeps it and later duplicates are
        // reassigned to a free palette color (via their probe order). Fixes the v1 collisions (two
        // folders sharing one "% 14" slot) without discarding the folder<->color mapping.
        std::vector<std::pair<std::wstring, std::wstring>> result;
        std::unordered_set<std::wstring> used; // palette colors already claimed
        std::vector<std::wstring> toReassign;
        for (const auto& [k, h] : entries)
        {
            if (!IsAutoPaletteColor(h))
            {
                result.emplace_back(k, h); // user pick — keep verbatim
            }
            else if (used.find(h) == used.end())
            {
                used.insert(h);
                result.emplace_back(k, h); // first folder to use this palette color keeps it
            }
            else
            {
                toReassign.push_back(k); // duplicate palette color — needs a fresh one
            }
        }
        for (const auto& k : toReassign)
        {
            const auto order = ColorProbeOrder(k, seed);
            std::wstring chosen = kAutoPalette[order.front()]; // fallback if the palette is full
            for (const size_t idx : order)
            {
                const std::wstring c = kAutoPalette[idx];
                if (used.find(c) == used.end())
                {
                    chosen = c;
                    break;
                }
            }
            used.insert(chosen);
            result.emplace_back(k, chosen);
        }
        return result;
    }

    void MigrateDirColorsToV2IfNeeded()
    {
        // One-time upgrade of dir-colors.json (v1 -> v2). v1 auto-assigned colors by a bare hash, so
        // distinct folders could COLLIDE on one palette slot (the reported "two folders, same color"
        // bug). v2 keeps the PERMANENT folder<->color mapping but DE-COLLIDES it: every folder keeps
        // its color where possible, and duplicate palette colors are reassigned to free ones (user
        // picks — off-palette — are preserved). Idempotent via the version stamp.
        std::lock_guard guard{ g_dirColorMtx };
        EnsureColorSeedLocked();
        const auto path = AgentmasterStateDir() + L"\\dir-colors.json";
        const auto text = ReadAllUtf8(path);
        if (text.empty())
        {
            return; // nothing persisted yet
        }
        const auto parsed = json::Parse(text);
        if (!parsed || parsed->NumAt(L"version", 1) >= 2)
        {
            return; // already migrated (or unreadable — leave it untouched)
        }
        SaveDirColors(DeCollideDirColors(DeserializeDirColors(text), g_colorSeed)); // rewrites at v2
    }

    std::wstring SerializeDirColors(const std::vector<std::pair<std::wstring, std::wstring>>& colors)
    {
        auto root = json::Value::MkObj();
        // v2: the PERMANENT folder<->color map — both auto-assigned and user-picked colors, persisted
        // so a folder keeps its color across restarts. v1 was the same shape but its auto colors could
        // collide; the one-time MigrateDirColorsToV2IfNeeded de-collides on first run.
        root.Set(L"version", json::Value::MkNum(2));
        auto arr = json::Value::MkArr();
        for (const auto& [dir, color] : colors)
        {
            auto o = json::Value::MkObj();
            o.Set(L"dir", json::Value::MkStr(dir));
            o.Set(L"color", json::Value::MkStr(color));
            arr.Push(std::move(o));
        }
        root.Set(L"colors", std::move(arr));
        return json::Dump(root);
    }

    std::vector<std::pair<std::wstring, std::wstring>> DeserializeDirColors(std::wstring_view text)
    {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return out;
        }
        if (const auto* arr = parsed->Find(L"colors"); arr && arr->type == json::Value::Type::Arr)
        {
            for (const auto& el : arr->arr)
            {
                if (el.type != json::Value::Type::Obj)
                {
                    continue;
                }
                const auto dir = el.StrAt(L"dir");
                const auto color = el.StrAt(L"color");
                if (!dir.empty() && !color.empty())
                {
                    out.emplace_back(dir, color);
                }
            }
        }
        return out;
    }

    void SaveDirColors(const std::vector<std::pair<std::wstring, std::wstring>>& colors)
    {
        WriteAllUtf8(AgentmasterStateDir() + L"\\dir-colors.json", SerializeDirColors(colors));
    }

    std::vector<std::pair<std::wstring, std::wstring>> LoadDirColors()
    {
        return DeserializeDirColors(ReadAllUtf8(AgentmasterStateDir() + L"\\dir-colors.json"));
    }

    std::optional<std::wstring> GetDirColor(const std::wstring& dir)
    {
        const std::wstring key = NormDirKey(dir);
        std::lock_guard guard{ g_dirColorMtx };
        for (const auto& [k, color] : LoadDirColors())
        {
            if (k == key)
            {
                return color;
            }
        }
        return std::nullopt;
    }

    void SetDirColor(const std::wstring& dir, const std::optional<std::wstring>& colorHex)
    {
        const std::wstring key = NormDirKey(dir);
        std::lock_guard guard{ g_dirColorMtx };
        auto colors = LoadDirColors();
        // Drop any existing entry for this dir, then (when setting) append the new one — this both
        // upserts a color and, for nullopt (a reset), simply removes it.
        std::vector<std::pair<std::wstring, std::wstring>> kept;
        kept.reserve(colors.size() + 1);
        for (auto& entry : colors)
        {
            if (entry.first != key)
            {
                kept.push_back(std::move(entry));
            }
        }
        if (colorHex)
        {
            kept.emplace_back(key, *colorHex);
        }
        SaveDirColors(kept);
    }

    // --- per-directory env overrides (dir-env.json) ---------------------------------------------
    // Same shape + lifecycle as dir-colors.json: a NormDirKey -> env-text map, where env-text is the
    // multi-line NAME=VALUE block the Settings cog's Per-directory tab edits. Merged OVER the global
    // AppSettings.env at spawn time (MergeSessionEnv / ResolveSessionEnv). The cog is the only writer.
    std::wstring SerializeDirEnv(const std::vector<std::pair<std::wstring, std::wstring>>& entries)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
        auto arr = json::Value::MkArr();
        for (const auto& [dir, env] : entries)
        {
            auto o = json::Value::MkObj();
            o.Set(L"dir", json::Value::MkStr(dir));
            o.Set(L"env", json::Value::MkStr(env));
            arr.Push(std::move(o));
        }
        root.Set(L"dirs", std::move(arr));
        return json::Dump(root);
    }

    std::vector<std::pair<std::wstring, std::wstring>> DeserializeDirEnv(std::wstring_view text)
    {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        const auto parsed = json::Parse(text);
        if (!parsed)
        {
            return out;
        }
        if (const auto* arr = parsed->Find(L"dirs"); arr && arr->type == json::Value::Type::Arr)
        {
            for (const auto& el : arr->arr)
            {
                if (el.type != json::Value::Type::Obj)
                {
                    continue;
                }
                const auto dir = el.StrAt(L"dir");
                const auto env = el.StrAt(L"env");
                // A blank (empty OR whitespace-only) env carries no overrides — drop it (a cleared editor
                // removes the dir; matches SetDirEnv's blank check).
                bool blank = true;
                for (const wchar_t c : env)
                {
                    if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
                    {
                        blank = false;
                        break;
                    }
                }
                if (!dir.empty() && !blank)
                {
                    out.emplace_back(dir, env);
                }
            }
        }
        return out;
    }

    void SaveDirEnv(const std::vector<std::pair<std::wstring, std::wstring>>& entries)
    {
        WriteAllUtf8(AgentmasterStateDir() + L"\\dir-env.json", SerializeDirEnv(entries));
    }

    std::vector<std::pair<std::wstring, std::wstring>> LoadDirEnv()
    {
        return DeserializeDirEnv(ReadAllUtf8(AgentmasterStateDir() + L"\\dir-env.json"));
    }

    std::wstring GetDirEnv(const std::wstring& dir)
    {
        const std::wstring key = NormDirKey(dir);
        std::lock_guard guard{ g_dirEnvMtx };
        for (const auto& [k, env] : LoadDirEnv())
        {
            if (k == key)
            {
                return env;
            }
        }
        return {};
    }

    void SetDirEnv(const std::wstring& dir, std::wstring_view envText)
    {
        const std::wstring key = NormDirKey(dir);
        std::lock_guard guard{ g_dirEnvMtx };
        auto entries = LoadDirEnv();
        std::vector<std::pair<std::wstring, std::wstring>> kept;
        kept.reserve(entries.size() + 1);
        for (auto& e : entries)
        {
            if (e.first != key)
            {
                kept.push_back(std::move(e));
            }
        }
        // A non-blank block upserts; a blank one simply removes the dir's entry.
        bool blank = true;
        for (const wchar_t c : envText)
        {
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
            {
                blank = false;
                break;
            }
        }
        if (!blank)
        {
            kept.emplace_back(key, std::wstring{ envText });
        }
        SaveDirEnv(kept);
    }

    // --- the Claude USER settings.json repository (ENV_VARS.md §8) -------------------------------
    // The user's GLOBAL ~/.claude/settings.json (NOT Agentmaster's own settings.json). cleanupPeriodDays
    // (history retention) lives here; the cog's "Keep Claude history (days)" field reads/writes it.

    static std::mutex& ClaudeUserSettingsMtx()
    {
        static std::mutex m;
        return m;
    }

    // Pretty-print a json::Value with 2-space indentation, so our RMW leaves the user's hand-editable
    // settings.json readable (json::Dump is a single line). Scalars reuse the shared compact dump.
    static void DumpJsonPretty(const json::Value& v, std::wstring& out, int depth)
    {
        const auto indent = [&out](int d) {
            for (int k = 0; k < d; ++k)
            {
                out += L"  ";
            }
        };
        if (v.type == json::Value::Type::Obj)
        {
            if (v.members.empty())
            {
                out += L"{}";
                return;
            }
            out += L"{\n";
            for (size_t k = 0; k < v.members.size(); ++k)
            {
                indent(depth + 1);
                json::detail::Escape(v.members[k].first, out);
                out += L": ";
                DumpJsonPretty(v.members[k].second, out, depth + 1);
                if (k + 1 < v.members.size())
                {
                    out += L',';
                }
                out += L'\n';
            }
            indent(depth);
            out += L'}';
        }
        else if (v.type == json::Value::Type::Arr)
        {
            if (v.arr.empty())
            {
                out += L"[]";
                return;
            }
            out += L"[\n";
            for (size_t k = 0; k < v.arr.size(); ++k)
            {
                indent(depth + 1);
                DumpJsonPretty(v.arr[k], out, depth + 1);
                if (k + 1 < v.arr.size())
                {
                    out += L',';
                }
                out += L'\n';
            }
            indent(depth);
            out += L']';
        }
        else
        {
            json::detail::Dump(v, out); // scalar: null / bool / number / string
        }
    }

    // Resolve <CLAUDE_CONFIG_DIR | %USERPROFILE%\.claude> (mirrors ClaudeProjectsDir's base). "" if unresolved.
    static std::wstring ClaudeConfigBase()
    {
        const auto env = [](const wchar_t* n) -> std::wstring {
            const DWORD need = ::GetEnvironmentVariableW(n, nullptr, 0);
            if (need == 0)
            {
                return {};
            }
            std::wstring buf(need, L'\0');
            const DWORD got = ::GetEnvironmentVariableW(n, buf.data(), need);
            if (got == 0 || got >= need)
            {
                return {};
            }
            buf.resize(got);
            return buf;
        };
        std::wstring base = env(L"CLAUDE_CONFIG_DIR");
        if (base.empty())
        {
            const std::wstring home = env(L"USERPROFILE");
            if (home.empty())
            {
                return {};
            }
            base = home + L"\\.claude";
        }
        return base;
    }

    std::optional<std::wstring> UpsertJsonNumberKey(std::wstring_view existing, std::wstring_view key, std::optional<double> value)
    {
        const std::wstring keyStr{ key };
        json::Value root = json::Value::MkObj();
        bool hasContent = false;
        for (const wchar_t c : existing)
        {
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
            {
                hasContent = true;
                break;
            }
        }
        if (hasContent)
        {
            const auto parsed = json::Parse(existing);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                return std::nullopt; // non-empty but not an object => refuse to clobber the user's file
            }
            root = *parsed;
        }
        // Drop any existing instance of the key (preserving the order of the rest), then re-add when setting.
        auto& m = root.members;
        m.erase(std::remove_if(m.begin(), m.end(), [&keyStr](const std::pair<std::wstring, json::Value>& kv) { return kv.first == keyStr; }), m.end());
        if (value)
        {
            root.Set(keyStr, json::Value::MkNum(*value));
        }
        std::wstring out;
        DumpJsonPretty(root, out, 0);
        out += L'\n';
        return out;
    }

    std::wstring ClaudeUserSettingsPath()
    {
        const std::wstring base = ClaudeConfigBase();
        return base.empty() ? std::wstring{} : (base + L"\\settings.json");
    }

    std::optional<int64_t> GetClaudeCleanupPeriodDays()
    {
        std::lock_guard guard{ ClaudeUserSettingsMtx() };
        const std::wstring path = ClaudeUserSettingsPath();
        if (path.empty())
        {
            return std::nullopt;
        }
        const auto parsed = json::Parse(ReadAllUtf8(path));
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        const auto* mem = parsed->Find(L"cleanupPeriodDays");
        if (!mem || mem->type != json::Value::Type::Num)
        {
            return std::nullopt;
        }
        return mem->AsI64();
    }

    bool SetClaudeCleanupPeriodDays(std::optional<int64_t> days)
    {
        std::lock_guard guard{ ClaudeUserSettingsMtx() };
        const std::wstring path = ClaudeUserSettingsPath();
        if (path.empty())
        {
            return false;
        }
        const std::optional<double> v = days ? std::optional<double>{ static_cast<double>(*days) } : std::nullopt;
        const auto updated = UpsertJsonNumberKey(ReadAllUtf8(path), L"cleanupPeriodDays", v);
        if (!updated)
        {
            return false; // a non-empty file we couldn't parse — never overwrite it
        }
        return WriteAllUtf8(path, *updated);
    }

    void SeedSessionEnvDefaults()
    {
        auto s = LoadAppSettings();
        if (s.envDefaultsVersion >= kEnvDefaultsVersion)
        {
            return; // already seeded the current set; a default the user deleted stays gone
        }
        auto [newEnv, newVersion] = ApplyEnvDefaults(s.env, s.envDefaultsVersion);
        s.env = std::move(newEnv);
        s.envDefaultsVersion = newVersion;
        SaveAppSettings(s);
    }

    void SeedClaudeCleanupPeriodDaysIfNeeded()
    {
        auto s = LoadAppSettings();
        if (s.claudeCleanupDaysSeeded)
        {
            return; // seeded once already; respect a user who changed/removed it
        }
        // Only write a default when the user hasn't set cleanupPeriodDays themselves (don't override a
        // deliberate value). 36500 ~= 100 years => Claude effectively never purges global history. (0 would
        // be a footgun — in Claude it DISABLES transcript persistence entirely.)
        if (!GetClaudeCleanupPeriodDays().has_value())
        {
            SetClaudeCleanupPeriodDays(36500);
        }
        s.claudeCleanupDaysSeeded = true;
        SaveAppSettings(s);
    }

    void SaveLayout(const ManagerLayout& layout)
    {
        WriteAllUtf8(AgentmasterStateDir() + L"\\layout.json", SerializeLayout(layout));
    }
    ManagerLayout LoadLayout()
    {
        return DeserializeLayout(ReadAllUtf8(AgentmasterStateDir() + L"\\layout.json"));
    }
    void SaveAppSettings(const AppSettings& settings)
    {
        WriteAllUtf8(AgentmasterStateDir() + L"\\settings.json", SerializeAppSettings(settings));
    }
    AppSettings LoadAppSettings()
    {
        return DeserializeAppSettings(ReadAllUtf8(AgentmasterStateDir() + L"\\settings.json"));
    }

    // Per-window records live one-file-per-window under .agentmaster\windows\ so opening and
    // closing windows never contend on a single document.
    void SaveWindowRecord(const WindowRecord& record)
    {
        if (record.windowId.empty())
        {
            return;
        }
        const auto dir = AgentmasterStateDir() + L"\\windows";
        try
        {
            std::filesystem::create_directories(std::filesystem::path{ dir });
        }
        catch (...)
        {
        }
        WriteAllUtf8(dir + L"\\" + record.windowId + L".json", SerializeWindowRecord(record));
    }

    std::vector<WindowRecord> LoadWindowRecords()
    {
        std::vector<WindowRecord> out;
        try
        {
            const std::filesystem::path dir{ AgentmasterStateDir() + L"\\windows" };
            if (!std::filesystem::exists(dir))
            {
                return out;
            }
            // Collect first, then sort by filename so the order is CANONICAL and STABLE across calls.
            // The WindowEmperor scans this same dir (sorted identically) to map a windowId -> its
            // `-s <idx>` and a reopened window resolves records[idx] for geometry; both must agree on
            // the index, so a deterministic order is load-bearing (PERSISTENCE.md §13.5). directory_
            // iterator order is unspecified, hence the explicit sort.
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator{ dir })
            {
                if (entry.is_regular_file() && entry.path().extension() == L".json")
                {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
                return a.filename().wstring() < b.filename().wstring();
            });
            for (const auto& f : files)
            {
                auto w = DeserializeWindowRecord(ReadAllUtf8(f.wstring()));
                if (!w.windowId.empty())
                {
                    out.push_back(std::move(w));
                }
            }
        }
        catch (...)
        {
        }
        return out;
    }

    std::optional<WindowRecord> LoadWindowRecord(const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return std::nullopt;
        }
        try
        {
            const auto path = AgentmasterStateDir() + L"\\windows\\" + windowId + L".json";
            if (!std::filesystem::exists(std::filesystem::path{ path }))
            {
                return std::nullopt;
            }
            auto w = DeserializeWindowRecord(ReadAllUtf8(path));
            if (!w.windowId.empty())
            {
                return w;
            }
        }
        catch (...)
        {
        }
        return std::nullopt;
    }

    void DeleteWindowRecord(const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return;
        }
        try
        {
            std::filesystem::remove(std::filesystem::path{ AgentmasterStateDir() + L"\\windows\\" + windowId + L".json" });
        }
        catch (...)
        {
        }
    }

    // ---- templates apply ----

    PlanTemplate MakeTemplateFromQueue(const std::wstring& name, const std::vector<QueuedPrompt>& queue)
    {
        PlanTemplate t;
        t.name = name;
        for (const auto& src : queue)
        {
            QueuedPrompt p;
            p.label = src.label;
            p.text = src.text;
            p.gate = src.gate;
            p.delayMs = src.delayMs;
            p.guardPattern = src.guardPattern;
            p.maxAttempts = src.maxAttempts;
            // id/status/attempts/sentAt are intentionally reset (assigned on apply).
            t.prompts.push_back(std::move(p));
        }
        return t;
    }

    void AppendTemplateToQueue(std::vector<QueuedPrompt>& queue, const PlanTemplate& tmpl)
    {
        for (const auto& src : tmpl.prompts)
        {
            QueuedPrompt p;
            p.id = NewSessionId(); // fresh id so it's unique within the target queue
            p.label = src.label;
            p.text = src.text;
            p.gate = src.gate;
            p.delayMs = src.delayMs;
            p.guardPattern = src.guardPattern;
            p.maxAttempts = src.maxAttempts;
            p.status = PromptStatus::Pending; // never carry a Sent status into a fresh apply
            queue.push_back(std::move(p));
        }
    }
}
