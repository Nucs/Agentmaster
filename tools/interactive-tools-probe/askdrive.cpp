// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// askdrive — the live end-to-end driver of MESSAGING.md §21.30 / INTERACTIVE_TOOLS.md §10.4 (a test tool, not part of the
// product; built by build_askdrive.py from the engine sources the m5 suite compiles).
//
// It plays Agentmaster's ENGINE side with the real code: it writes the real question hook script (BuildAskHookScript) and the
// real hooks settings file (BuildHooksSettingsJson, exec form, with the PermissionRequest entry) into <dir>, runs the real
// AskBridge + AskRegistry on \\.\pipe\<base>.ask, and answers each question hook's request with a decision the real
// AskQuestion.h builds — the very lines the Telegram card sends. The Python side (askprobe.py) drives a real claude.exe whose
// --settings is that file and whose CCMGR_HOOK_PIPE names \\.\pipe\<base>, so the hook that runs is the shipped PowerShell script.
//
// Usage: askdrive.exe <base> <dir> <action> [<action> ...]
//   one action per question request, in order:
//     pick:<option index>[:<note>]   the option (and a note) — what tapping an option (after 📝) sends
//     notes:<note>                   a note alone ("(notes only)")
//     own:<text>                     the user's own answer (✍️)
//     release                        "{}": the dialog stays with the PC (the probe answers it there)
//     chat                           Chat about this (the PC's rejection with its clarify request)
//     hold                           no answer: the helper keeps waiting (askmem.py measures it, askparent.py ends its parent)
// Every request and decision goes to <dir>\drive.jsonl; the driver exits when every action's request has finished (the hook
// got its reply, or went away), or after 15 minutes, or when <dir>\stop exists.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../src/cascadia/TerminalApp/AgentMaster/AskQuestion.h"
#include "../../src/cascadia/TerminalApp/AgentMaster/ClaudeSpawn.h"
#include "../../src/cascadia/TerminalApp/AgentMaster/HooksBridge.h"
#include "../../src/cascadia/TerminalApp/AgentMaster/Json.h"

namespace
{
    std::string Utf8(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring Wide(const char* s)
    {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::wstring out(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
        return out;
    }

    void WriteFileBytes(const std::wstring& path, const std::string& bytes)
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void Log(const std::wstring& dir, const std::wstring& jsonLine)
    {
        std::ofstream f(dir + L"\\drive.jsonl", std::ios::binary | std::ios::app);
        const std::string b = Utf8(jsonLine) + "\n";
        f.write(b.data(), static_cast<std::streamsize>(b.size()));
        std::fwprintf(stdout, L"%ls\n", jsonLine.c_str());
        std::fflush(stdout);
    }

    std::wstring Q(const std::wstring& s)
    {
        return Agentmaster::Msg::AskJsonQuote(s);
    }
}

int wmain(int argc, wchar_t** argv)
{
    using namespace Agentmaster;
    using namespace Agentmaster::Msg;
    if (argc < 4)
    {
        std::fwprintf(stderr, L"usage: askdrive <base> <dir> <action> [...]\n");
        return 2;
    }
    const std::wstring base = argv[1];
    const std::wstring dir = argv[2];
    std::vector<std::wstring> actions;
    for (int i = 3; i < argc; ++i)
    {
        actions.emplace_back(argv[i]);
    }
    // The real files, as MaterializeSharedHookFiles writes them (a no-op forwarder stands in for agentmaster-hook.ps1).
    const std::wstring askPath = dir + L"\\agentmaster-ask.ps1";
    WriteFileBytes(askPath, "\xEF\xBB\xBF" + Utf8(BuildAskHookScript(dir)));
    const std::wstring fwdPath = dir + L"\\agentmaster-hook.ps1";
    WriteFileBytes(fwdPath, "exit 0\r\n");
    wchar_t sys[MAX_PATH]{};
    ::GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring host = std::wstring{ sys } + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const auto fwd = [](std::wstring p) {
        for (auto& c : p)
        {
            c = c == L'\\' ? L'/' : c;
        }
        return p;
    };
    WriteFileBytes(dir + L"\\settings.json", Utf8(BuildHooksSettingsJson(fwd(fwdPath), L"", true, true, host, fwd(askPath))));

    auto reg = std::make_shared<AskRegistry>();
    reg->SetAcceptor([](const std::wstring&) { return true; });
    AskBridge bridge{ L"\\\\.\\pipe\\" + base + L".ask", reg };
    if (!bridge.Start())
    {
        std::fwprintf(stderr, L"the bridge did not start\n");
        return 3;
    }
    Log(dir, L"{\"ev\":\"ready\",\"pipe\":" + Q(L"\\\\.\\pipe\\" + base + L".ask") + L"}");

    size_t next = 0;
    std::vector<uint64_t> handled;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;)
    {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(15) || ::GetFileAttributesW((dir + L"\\stop").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            Log(dir, L"{\"ev\":\"stop\"}");
            break;
        }
        const auto entries = reg->Snapshot();
        bool allDone = next >= actions.size();
        for (const auto& e : entries)
        {
            const bool seen = std::find(handled.begin(), handled.end(), e.id) != handled.end();
            if (!seen && e.state == AskEntryState::Open && next < actions.size())
            {
                handled.push_back(e.id);
                const std::wstring action = actions[next++];
                auto req = ParseAskRequestLine(e.requestLine);
                if (!req || req->questions.empty())
                {
                    reg->Release(e.id, L"unreadable");
                    Log(dir, L"{\"ev\":\"unreadable\",\"id\":" + std::to_wstring(e.id) + L"}");
                    continue;
                }
                AskCardModel m = MakeAskCardModel(req->questions);
                std::string line;
                std::wstring kind;
                if (action.rfind(L"pick:", 0) == 0)
                {
                    const std::wstring rest = action.substr(5);
                    const size_t colon = rest.find(L':');
                    m.choices[0].picks = { static_cast<size_t>(std::stoul(rest.substr(0, colon))) };
                    if (colon != std::wstring::npos)
                    {
                        m.choices[0].note = rest.substr(colon + 1);
                    }
                    kind = L"allow";
                }
                else if (action.rfind(L"notes:", 0) == 0)
                {
                    m.choices[0].note = action.substr(6);
                    kind = L"allow";
                }
                else if (action.rfind(L"own:", 0) == 0)
                {
                    m.choices[0].otherText = action.substr(4);
                    kind = L"allow";
                }
                else if (action == L"chat")
                {
                    line = BuildAskDenyLine(AskChatAboutThisMessage(m.questions, m.choices), false);
                    kind = L"deny";
                }
                else if (action == L"hold")
                {
                    // Never answered: the helper keeps waiting (its memory can be measured) until the stop file ends the driver,
                    // whose bridge Stop() hands it "{}".
                    Log(dir, L"{\"ev\":\"holding\",\"id\":" + std::to_wstring(e.id) + L",\"hookPid\":" + std::to_wstring(req->hookPid) + L"}");
                    continue;
                }
                else
                {
                    kind = L"release";
                }
                if (kind == L"allow")
                {
                    json::Value answers;
                    json::Value annotations;
                    ComposeAskAnswers(m.questions, m.choices, answers, annotations);
                    line = BuildAskAllowLine(req->toolInput, answers, annotations);
                }
                bool ok = false;
                if (kind == L"release")
                {
                    ok = reg->Release(e.id, L"released by the probe");
                }
                else
                {
                    ok = reg->Answer(e.id, line);
                }
                Log(dir, L"{\"ev\":\"decided\",\"id\":" + std::to_wstring(e.id) + L",\"action\":" + Q(action) + L",\"kind\":" + Q(kind) + L",\"ok\":" +
                             (ok ? L"true" : L"false") + L",\"session\":" + Q(req->sessionId) + L",\"hookPid\":" + std::to_wstring(req->hookPid) +
                             L",\"claudePid\":" + std::to_wstring(req->claudePid) + L",\"signature\":" + Q(AskQuestionsSignature(m.questions)) +
                             L",\"previewLayout\":" + (m.questions[0].previewLayout ? L"true" : L"false") + L",\"line\":" + Q(Wide(line.c_str())) + L"}");
            }
        }
        // Done when every action was used and every handled request finished (its reply written, or its hook gone).
        if (allDone)
        {
            bool finished = true;
            for (const auto& e : entries)
            {
                if (std::find(handled.begin(), handled.end(), e.id) != handled.end() && (e.state == AskEntryState::Open || e.state == AskEntryState::Answered))
                {
                    finished = false;
                }
            }
            if (finished)
            {
                for (const auto& e : entries)
                {
                    const wchar_t* st = e.state == AskEntryState::Delivered ? L"delivered" : e.state == AskEntryState::Released ? L"released" : e.state == AskEntryState::Gone ? L"gone" : L"other";
                    Log(dir, L"{\"ev\":\"final\",\"id\":" + std::to_wstring(e.id) + L",\"state\":" + Q(st) + L",\"why\":" + Q(e.why) + L"}");
                }
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    bridge.Stop();
    return 0;
}
