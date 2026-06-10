// Agentmaster — ad-hoc diagnostic for Bring Window To Front (NOT part of the test suite).
// 1) Census: every claude.exe with its cwd / WT_SESSION / AM_SESSION / resolved session id.
// 2) Every visible WT-class (CASCADIA_HOSTING_WINDOW_CLASS) top-level window, its UIA TabItems,
//    and how each tab name scores against the hints the REAL picker would use (title hints +
//    cwd leaf + transcript token corpus — pass cwd+sessionId to reproduce a row's pick).
// 3) Optional LIVE run: `uia_probe.exe <cwd> <sessionId> <titleHint> bring <claudePid>` invokes
//    the real BringClaudeWindowToFront (foregrounds a window!).
// Build + run: _run-uia-probe.bat [cwd] [sessionId] [titleHint]
#include "../ProcessInspect.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace Agentmaster;

int wmain(int argc, wchar_t** argv)
{
    const std::wstring cwd = argc > 1 ? argv[1] : L"";
    const std::wstring sessionId = argc > 2 ? argv[2] : L"";
    const std::wstring titleHint = argc > 3 ? argv[3] : L"";
    const bool live = argc > 5 && std::wstring{ argv[4] } == L"bring";

    // --- the hints the real picker builds (mirror of BringClaudeWindowToFront) ---
    std::vector<std::wstring> titles;
    if (!titleHint.empty())
    {
        titles.push_back(titleHint);
    }
    std::unordered_set<std::wstring> headTokens, fullTokens;
    const std::wstring cwdLeaf = PathLeaf(cwd);
    if (!cwd.empty() && !sessionId.empty())
    {
        const auto ti = ReadTranscriptInfo(cwd, sessionId, 2 * 1024 * 1024, 200);
        wprintf(L"transcript: found=%d customTitle=\"%ls\" title=\"%.60ls\" prompts=%zu\n",
                ti.found ? 1 : 0, ti.customTitle.c_str(), ti.title.c_str(), ti.userPrompts.size());
        if (!ti.customTitle.empty())
        {
            titles.push_back(ti.customTitle);
        }
        if (!ti.title.empty())
        {
            titles.push_back(ti.title);
        }
        std::wstring head{ ti.title };
        head += L'\n';
        head += ti.customTitle;
        if (!ti.userPrompts.empty())
        {
            head += L'\n';
            head += ti.userPrompts.front();
        }
        std::wstring full{ head };
        for (size_t i = 1; i < ti.userPrompts.size(); ++i)
        {
            full += L'\n';
            full += ti.userPrompts[i];
        }
        headTokens = TokenizeTextLower(head);
        fullTokens = TokenizeTextLower(full);
        wprintf(L"corpus tokens: head=%zu full=%zu\n", headTokens.size(), fullTokens.size());
    }
    const auto scoreTab = [&](const std::wstring& name) {
        int best = titles.empty() ? ScoreClaudeTabName(name, L"", cwdLeaf) : 0;
        for (const auto& t : titles)
        {
            const int s = ScoreClaudeTabName(name, t, cwdLeaf);
            best = s > best ? s : best;
        }
        if (ScoreTabNameTokens(name, headTokens) > 0)
        {
            best = 50 > best ? 50 : best;
        }
        else if (ScoreTabNameTokens(name, fullTokens) > 0)
        {
            best = 45 > best ? 45 : best;
        }
        return best;
    };

    // --- claude census (pids to feed back into this tool) ---
    const auto snap = SnapshotProcesses();
    wprintf(L"\n-- claude census --\n");
    for (const auto& e : snap)
    {
        if (!ImageNameEq(e.image, L"claude.exe"))
        {
            continue;
        }
        const auto f = ReadClaudeFacts(e.pid);
        const auto sid = ResolveSessionId(f.cwd, f.startUnixMs);
        wprintf(L"claude pid=%u ppid=%u cwd=%ls wt=%.8ls am=%.8ls sid=%.8ls\n",
                e.pid, e.ppid, f.cwd.c_str(), f.wtSession.c_str(), f.amSession.c_str(), sid.c_str());
    }

    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    {
        ComPtr<IUIAutomation> uia;
        HRESULT hr = ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
        wprintf(L"\nCoCreateInstance(CUIAutomation) hr=0x%08lX\n", static_cast<unsigned long>(hr));

        struct Win
        {
            HWND h;
            DWORD pid;
            std::wstring title;
        };
        std::vector<Win> wins;
        ::EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                auto& v = *reinterpret_cast<std::vector<Win>*>(lp);
                if (!::IsWindowVisible(h) || ::GetWindow(h, GW_OWNER) != nullptr)
                {
                    return TRUE;
                }
                wchar_t cls[128]{};
                ::GetClassNameW(h, cls, 128);
                if (::wcsstr(cls, L"CASCADIA") == nullptr)
                {
                    return TRUE;
                }
                DWORD pid = 0;
                ::GetWindowThreadProcessId(h, &pid);
                wchar_t title[512]{};
                ::GetWindowTextW(h, title, 512);
                v.push_back(Win{ h, pid, title });
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&wins));

        wprintf(L"%zu WT-class window(s)\n", wins.size());
        for (const auto& w : wins)
        {
            wprintf(L"\n== hwnd=%p pid=%lu window title=\"%ls\"\n", static_cast<void*>(w.h), w.pid, w.title.c_str());
            ComPtr<IUIAutomationElement> root;
            if (FAILED(uia->ElementFromHandle(w.h, &root)) || !root)
            {
                wprintf(L"   ElementFromHandle FAILED\n");
                continue;
            }
            VARIANT v{};
            v.vt = VT_I4;
            v.lVal = UIA_TabItemControlTypeId;
            ComPtr<IUIAutomationCondition> cond;
            uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
            ComPtr<IUIAutomationElementArray> found;
            HRESULT fhr = root->FindAll(TreeScope_Descendants, cond.Get(), &found);
            int n = 0;
            if (found)
            {
                found->get_Length(&n);
            }
            wprintf(L"   FindAll(TabItem) hr=0x%08lX count=%d\n", static_cast<unsigned long>(fhr), n);
            std::vector<std::wstring> names;
            for (int i = 0; i < n; ++i)
            {
                ComPtr<IUIAutomationElement> el;
                std::wstring name;
                if (SUCCEEDED(found->GetElement(i, &el)) && el)
                {
                    BSTR b = nullptr;
                    if (SUCCEEDED(el->get_CurrentName(&b)) && b)
                    {
                        name.assign(b, ::SysStringLen(b));
                        ::SysFreeString(b);
                    }
                }
                wprintf(L"   tab[%d] rawScore=%3d name=\"%ls\"\n", i, scoreTab(name), name.c_str());
                names.push_back(std::move(name));
            }
            // The REAL pick (raw scores above lack the subset-disqualifier / unique guard).
            int score = 0;
            bool unique = false;
            const int pick = PickClaudeTab(names, titles, cwdLeaf, headTokens, fullTokens, score, unique);
            wprintf(L"   -> PickClaudeTab: idx=%d score=%d unique=%d (selects only when idx>=0 && unique)\n", pick, score, unique ? 1 : 0);
        }
    }
    ::CoUninitialize();

    if (live)
    {
        const uint32_t pid = static_cast<uint32_t>(_wtoi(argv[5]));
        wprintf(L"\n-- LIVE BringClaudeWindowToFront(pid=%u) --\n", pid);
        const bool ok = BringClaudeWindowToFront(pid, 0, sessionId, titleHint, cwd);
        wprintf(L"returned %ls\n", ok ? L"true" : L"false");
    }
    return 0;
}
