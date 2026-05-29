// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
#include "Persistence.h"

#include "ClaudeSpawn.h" // AgentmasterStateDir, NewSessionId

#include <windows.h>

#include <filesystem>
#include <fstream>

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
