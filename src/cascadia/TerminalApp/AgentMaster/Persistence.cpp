// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
#include "Persistence.h"

#include "ClaudeSpawn.h" // AgentmasterStateDir, NewSessionId

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
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
        try
        {
            std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                return false;
            }
            const auto bytes = Utf16ToUtf8(content);
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return f.good();
        }
        catch (...)
        {
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

    std::wstring ToString(AutopilotMode m)
    {
        switch (m)
        {
        case AutopilotMode::SemiAuto:
            return L"SemiAuto";
        case AutopilotMode::Full:
            return L"Full";
        case AutopilotMode::Off:
        default:
            return L"Off";
        }
    }
    AutopilotMode AutopilotModeFromString(std::wstring_view s)
    {
        if (s == L"SemiAuto")
            return AutopilotMode::SemiAuto;
        if (s == L"Full")
            return AutopilotMode::Full;
        return AutopilotMode::Off;
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
        return o == PromptOrigin::Typed ? L"Typed" : L"Flight";
    }
    PromptOrigin PromptOriginFromString(std::wstring_view s)
    {
        return s == L"Typed" ? PromptOrigin::Typed : PromptOrigin::Flight;
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
        p.origin = PromptOriginFromString(v.StrAt(L"origin", L"Flight"));
        // `echoed` is transient (not persisted): a reloaded Sent prompt's echo already
        // happened in a past run; the recency window stops it from matching a fresh message.
        return p;
    }

    json::Value ToJson(const AutopilotState& a)
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

    AutopilotState AutopilotFromJson(const json::Value& v)
    {
        AutopilotState a;
        a.mode = AutopilotModeFromString(v.StrAt(L"mode", L"Off"));
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
        o.Set(L"state", json::Value::MkStr(ToString(s.state)));
        o.Set(L"lastActivityUnixMs", json::Value::MkNum(static_cast<double>(s.lastActivityUnixMs)));
        o.Set(L"external", json::Value::MkBool(s.external));
        auto q = json::Value::MkArr();
        for (const auto& p : s.queue)
        {
            q.Push(ToJson(p));
        }
        o.Set(L"queue", std::move(q));
        o.Set(L"autopilot", ToJson(s.autopilot));
        return o;
    }

    SessionInfo SessionFromJson(const json::Value& v)
    {
        SessionInfo s;
        s.id = v.StrAt(L"id");
        s.title = v.StrAt(L"title");
        s.workingDir = v.StrAt(L"workingDir");
        s.branch = v.StrAt(L"branch");
        s.state = SessionStateFromString(v.StrAt(L"state", L"Idle"));
        s.lastActivityUnixMs = v.I64At(L"lastActivityUnixMs");
        s.external = v.BoolAt(L"external", false);
        if (const auto* q = v.Find(L"queue"); q && q->type == json::Value::Type::Arr)
        {
            for (const auto& pv : q->arr)
            {
                s.queue.push_back(PromptFromJson(pv));
            }
        }
        if (const auto* a = v.Find(L"autopilot"); a && a->type == json::Value::Type::Obj)
        {
            s.autopilot = AutopilotFromJson(*a);
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
        o.Set(L"defaultAutopilotMode", json::Value::MkStr(ToString(s.defaultAutopilotMode)));
        o.Set(L"maxAutoSends", json::Value::MkNum(s.maxAutoSends));
        o.Set(L"stopOnError", json::Value::MkBool(s.stopOnError));
        o.Set(L"pauseOnHumanInput", json::Value::MkBool(s.pauseOnHumanInput));
        o.Set(L"confirmBeforeKill", json::Value::MkBool(s.confirmBeforeKill));
        o.Set(L"defaultLaunchDir", json::Value::MkStr(s.defaultLaunchDir));
        o.Set(L"recentDirsLimit", json::Value::MkNum(s.recentDirsLimit));
        o.Set(L"showTabOverlay", json::Value::MkBool(s.showTabOverlay));
        return o;
    }

    AppSettings AppSettingsFromJson(const json::Value& v)
    {
        AppSettings s; // any missing field keeps the struct default (== prior hardcoded behavior)
        s.skipPermissions = v.BoolAt(L"skipPermissions", true);
        s.model = v.StrAt(L"model");
        s.includeCoAuthoredBy = v.BoolAt(L"includeCoAuthoredBy", true);
        s.env = v.StrAt(L"env");
        s.defaultAutopilotMode = AutopilotModeFromString(v.StrAt(L"defaultAutopilotMode", L"Off"));
        s.maxAutoSends = v.U32At(L"maxAutoSends", 100);
        s.stopOnError = v.BoolAt(L"stopOnError", true);
        s.pauseOnHumanInput = v.BoolAt(L"pauseOnHumanInput", true);
        s.confirmBeforeKill = v.BoolAt(L"confirmBeforeKill", true);
        s.defaultLaunchDir = v.StrAt(L"defaultLaunchDir");
        s.recentDirsLimit = v.U32At(L"recentDirsLimit", 10);
        s.showTabOverlay = v.BoolAt(L"showTabOverlay", true);
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
        o.Set(L"kind", json::Value::MkStr(t.kind == TabKind::Other ? L"Other" : L"Claude"));
        if (t.kind == TabKind::Claude)
        {
            o.Set(L"sessionId", json::Value::MkStr(t.sessionId)); // a reference; the record lives in sessions.json
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
        t.kind = (v.StrAt(L"kind", L"Claude") == L"Other") ? TabKind::Other : TabKind::Claude;
        if (t.kind == TabKind::Claude)
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
        return o;
    }

    ManagerState ManagerStateFromJson(const json::Value& v)
    {
        ManagerState m;
        m.selectedId = v.StrAt(L"selectedId");
        m.scopeDir = v.StrAt(L"scopeDir");
        m.selectedPromptId = v.StrAt(L"selectedPromptId");
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
        o.Set(L"manager", ToJson(w.manager));
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

    std::wstring AutoDirColorHex(const std::wstring& dir)
    {
        // A fixed palette of distinct, readable tab colors. Same dir => same color (hash of the
        // canonical key); different dirs spread across the palette.
        static const wchar_t* const kPalette[] = {
            L"#E06C75", L"#E5C07B", L"#98C379", L"#56B6C2", L"#61AFEF", L"#C678DD",
            L"#D19A66", L"#BE5046", L"#528BFF", L"#7FD962", L"#FF9E64", L"#2BBAC5",
            L"#B267E6", L"#F78C6C",
        };
        const std::wstring key = NormDirKey(dir);
        // FNV-1a over the key's code units (stable across runs).
        uint64_t h = 1469598103934665603ull;
        for (const wchar_t c : key)
        {
            h ^= static_cast<uint64_t>(static_cast<uint16_t>(c));
            h *= 1099511628211ull;
        }
        const size_t n = sizeof(kPalette) / sizeof(kPalette[0]);
        return kPalette[static_cast<size_t>(h % n)];
    }

    std::wstring SerializeDirColors(const std::vector<std::pair<std::wstring, std::wstring>>& colors)
    {
        auto root = json::Value::MkObj();
        root.Set(L"version", json::Value::MkNum(1));
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
