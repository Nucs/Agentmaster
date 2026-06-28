// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster ProcessInspect -- Fleet Observer primitives (6 partial files)
// Out-of-band process/transcript inspection (OBSERVER.md): PEB reads, transcript resolution +
// content, Codex rollout, the session-end.js summary analyzer, and window activation. Plain C++
// (no WinRT/PCH). Split from the former 4950-line ProcessInspect.cpp by section; all share the
// ProcessInspect.Internal.h primitives. The public API is declared in ProcessInspect.h.
//
// Partial files in this group (★ marks THIS file):
//   ProcessInspect.cpp             - CORE: process enumeration (Toolhelp) + PEB facts read/classify
//   ProcessInspect.Internal.h      - shared file-local primitives: x64 PEB reads, string/file helpers, transcript globbing (anonymous namespace, a per-TU copy)
//   ProcessInspect.Transcript.cpp  - Claude transcript resolution + timing/title/prompts + git plumbing + Codex (C1) facts/rollout
//   ProcessInspect.Content.cpp     - conversation-text read + the session-end.js summary analyzer + Codex (C2) rollout-tail state
// ★ ProcessInspect.Window.cpp      - Bring Window To Front (its own UIA-helper anon ns + the public window/tab-pick API)
//   ProcessInspect.Summary.cpp     - the shared summary-box renderers (the per-tab overlay + the Sessions page)
// ======================================================================================
//
// Agentmaster engine TU. ProcessInspect WINDOW: Bring Window To Front -- locate + surface a foreign claude/codex host window out-of-band, best-effort WT tab pick via UI Automation. Carries its OWN anonymous namespace of window/UIA helpers. Partial TU of ProcessInspect.cpp.
#include "ProcessInspect.h"

#include <windows.h>
#include <appmodel.h> // GetPackageFamilyName (host terminal: real WT vs Agentmaster vs Dev)
#include <tlhelp32.h> // CreateToolhelp32Snapshot

// Bring Window To Front (BringClaudeWindowToFront): COM + UI Automation client for the WT tab
// pick. Raw COM via WRL ComPtr — still no WinRT, still standalone-harness friendly. The explicit
// objbase/oleauto includes keep this TU independent of WIN32_LEAN_AND_MEAN trimming windows.h.
#include <objbase.h> // CoInitializeEx / CoCreateInstance
#include <oleauto.h> // SysStringLen / SysFreeString (UIA names are BSTRs)
#include <UIAutomation.h> // IUIAutomation* (tab enumeration + SelectionItem.Select)
#include <wrl/client.h> // Microsoft::WRL::ComPtr

#include <algorithm>
#include <cwctype> // towlower (tab-name heuristics)
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "ClaudeSpawn.h" // ClaudeProjectsDir() — the live Claude transcript root
#include "Json.h" // transcript line parsing (ReadTranscriptInfo)
#include "TranscriptStore.h" // IsNoiseUserPrompt + PickDisplayTitle (the shared prompt-noise + title-precedence rules)
#include "ProcessInspect.Internal.h" // the shared file-local PEB/string/file primitives

// ============================================================================================
// Bring Window To Front (the Manager's EXTERNAL right-click, last item) — find + surface the
// top-level window that HOSTS a foreign claude, out-of-band: restore it when minimized,
// foreground it, and when the host is a Windows Terminal-class window best-effort select the
// claude's TAB via UI Automation. Window activation only — never console input (Rule #13).
// ============================================================================================

namespace
{
    // The visible terminal window class every Windows Terminal 1.x main window registers
    // (IslandWindow's XAML_HOSTING_WINDOW_CLASS_NAME) — ours included (the fork's PFN suffix is
    // on the Emperor's hidden MESSAGE-window class, not the island window). A window of this
    // class is "WT-like": it carries a tab strip whose TabItems UIA can enumerate + select.
    constexpr std::wstring_view kTerminalIslandClass = L"CASCADIA_HOSTING_WINDOW_CLASS";

    std::wstring LowerCopy(std::wstring_view s)
    {
        std::wstring r{ s };
        for (auto& c : r)
        {
            c = static_cast<wchar_t>(::towlower(c));
        }
        return r;
    }

    bool IsTerminalIslandWindow(HWND h)
    {
        wchar_t cls[64]{};
        const int n = ::GetClassNameW(h, cls, ARRAYSIZE(cls));
        return n > 0 && std::wstring_view{ cls, static_cast<size_t>(n) } == kTerminalIslandClass;
    }

    // All VISIBLE, unowned top-level windows of `pid`, in z-order (EnumWindows order, topmost
    // first). Owned popups/tool windows are skipped. A minimized window is still WS_VISIBLE, so
    // it IS found — restoring it is the whole point. (A headless ConPTY host / the Emperor's
    // message window are not visible and never match.)
    std::vector<HWND> TopLevelWindowsOfPid(uint32_t pid)
    {
        struct Ctx
        {
            uint32_t pid;
            std::vector<HWND> wins;
        } ctx{ pid, {} };
        ::EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                auto& c = *reinterpret_cast<Ctx*>(lp);
                DWORD wpid = 0;
                ::GetWindowThreadProcessId(h, &wpid);
                if (wpid == c.pid && ::IsWindowVisible(h) && ::GetWindow(h, GW_OWNER) == nullptr)
                {
                    c.wins.push_back(h);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        return ctx.wins;
    }

    // Every foreign (not-our-process) visible WT-class window, z-order topmost first. The
    // default-terminal-handoff fallback scans these; our OWN windows are excluded — an external
    // claude never lives in our roster (rostered == ours, Rule #13), and a managed claude tab of
    // ours could otherwise false-match the heuristics.
    std::vector<HWND> ForeignTerminalIslandWindows()
    {
        struct Ctx
        {
            DWORD selfPid;
            std::vector<HWND> wins;
        } ctx{ ::GetCurrentProcessId(), {} };
        ::EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                auto& c = *reinterpret_cast<Ctx*>(lp);
                DWORD wpid = 0;
                ::GetWindowThreadProcessId(h, &wpid);
                if (wpid != c.selfPid && ::IsWindowVisible(h) && ::GetWindow(h, GW_OWNER) == nullptr && IsTerminalIslandWindow(h))
                {
                    c.wins.push_back(h);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        return ctx.wins;
    }

    // Images the host-window ancestor walk stops AT: nothing meaningful for a terminal tab lives
    // above these, and explorer.exe OWNS windows we must never foreground (the desktop/taskbar).
    bool IsAncestorBoundaryImage(std::wstring_view image)
    {
        static constexpr std::wstring_view kStop[] = {
            L"explorer.exe", L"svchost.exe", L"services.exe", L"wininit.exe", L"winlogon.exe",
            L"csrss.exe", L"smss.exe", L"userinit.exe", L"dwm.exe", L"sihost.exe"
        };
        for (const auto s : kStop)
        {
            if (Agentmaster::ImageNameEq(image, s))
            {
                return true;
            }
        }
        return false;
    }

    // Restore-if-minimized + take foreground. Runs while OUR window is the foreground window (a
    // Manager menu click), so SetForegroundWindow is permitted to hand foreground away;
    // SwitchToThisWindow is the belt-and-braces fallback when the shell denies it anyway.
    void RestoreAndForeground(HWND hwnd)
    {
        if (::IsIconic(hwnd))
        {
            ::ShowWindow(hwnd, SW_RESTORE);
        }
        ::SetForegroundWindow(hwnd);
        if (::GetForegroundWindow() != hwnd)
        {
            ::SwitchToThisWindow(hwnd, TRUE);
        }
    }

    // One TabItem of a WT-class window, as UIA exposes it: the element (kept for Select) + name.
    struct UiaTab
    {
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        std::wstring name;
    };

    // Enumerate the TabItem descendants of `hwnd`. Empty on ANY failure (an elevated target —
    // UIA is UIPI-blocked from a non-elevated client — or an island that isn't hydrated): the
    // caller then simply foregrounds without a tab pick.
    std::vector<UiaTab> UiaTabsOf(IUIAutomation* uia, HWND hwnd)
    {
        std::vector<UiaTab> tabs;
        if (uia == nullptr)
        {
            return tabs;
        }
        Microsoft::WRL::ComPtr<IUIAutomationElement> root;
        if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root)
        {
            return tabs;
        }
        VARIANT v{};
        v.vt = VT_I4;
        v.lVal = UIA_TabItemControlTypeId;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> cond;
        if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) || !cond)
        {
            return tabs;
        }
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> found;
        if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &found)) || !found)
        {
            return tabs;
        }
        int n = 0;
        if (FAILED(found->get_Length(&n)))
        {
            return tabs;
        }
        for (int i = 0; i < n; ++i)
        {
            Microsoft::WRL::ComPtr<IUIAutomationElement> el;
            if (FAILED(found->GetElement(i, &el)) || !el)
            {
                continue;
            }
            BSTR b = nullptr;
            std::wstring name;
            if (SUCCEEDED(el->get_CurrentName(&b)) && b != nullptr)
            {
                name.assign(b, ::SysStringLen(b));
                ::SysFreeString(b);
            }
            tabs.push_back(UiaTab{ std::move(el), std::move(name) });
        }
        return tabs;
    }

    // Everything a tab name can be matched against for ONE claude: the title hints (display title,
    // custom title, first prompt — equal/containment/head rules), the cwd leaf, and the tokenized
    // conversation corpus in two tiers (head = title + first prompt; full = every prompt).
    struct TabPickHints
    {
        std::vector<std::wstring> titles;
        std::wstring cwdLeaf;
        std::unordered_set<std::wstring> headTokens;
        std::unordered_set<std::wstring> fullTokens;
    };

    // UiaTab adapter over the pure pick (PickClaudeTab owns the scoring + the subset/unique rules).
    int BestClaudeTabIn(const std::vector<UiaTab>& tabs, const TabPickHints& h, int& outScore, bool& outUnique)
    {
        std::vector<std::wstring> names;
        names.reserve(tabs.size());
        for (const auto& t : tabs)
        {
            names.push_back(t.name);
        }
        return Agentmaster::PickClaudeTab(names, h.titles, h.cwdLeaf, h.headTokens, h.fullTokens, outScore, outUnique);
    }

    // Select one tab. TabViewItem exposes SelectionItem (the real path); Invoke is just-in-case.
    bool UiaSelectTab(const UiaTab& tab)
    {
        Microsoft::WRL::ComPtr<IUIAutomationSelectionItemPattern> sel;
        if (SUCCEEDED(tab.element->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&sel))) && sel)
        {
            return SUCCEEDED(sel->Select());
        }
        Microsoft::WRL::ComPtr<IUIAutomationInvokePattern> inv;
        if (SUCCEEDED(tab.element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv)
        {
            return SUCCEEDED(inv->Invoke());
        }
        return false;
    }
}

namespace Agentmaster
{
    std::wstring PathLeaf(std::wstring_view path)
    {
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        {
            path.remove_suffix(1);
        }
        const auto cut = path.find_last_of(L"\\/");
        return std::wstring{ cut == std::wstring_view::npos ? path : path.substr(cut + 1) };
    }

    std::unordered_set<std::wstring> TokenizeTextLower(std::wstring_view text)
    {
        std::unordered_set<std::wstring> tokens;
        std::wstring cur;
        const auto flush = [&]() {
            if (cur.size() >= 3) // "am" / "gh" / "of" are pure noise
            {
                tokens.insert(cur);
            }
            cur.clear();
        };
        for (const wchar_t raw : text)
        {
            const wchar_t c = static_cast<wchar_t>(::towlower(raw));
            if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))
            {
                cur.push_back(c);
            }
            else
            {
                flush();
            }
        }
        flush();
        return tokens;
    }

    int ScoreTabNameTokens(std::wstring_view tabName, const std::unordered_set<std::wstring>& corpusTokens)
    {
        if (corpusTokens.empty())
        {
            return 0;
        }
        const auto nameTokens = TokenizeTextLower(tabName);
        if (nameTokens.empty())
        {
            return 0;
        }
        for (const auto& t : nameTokens)
        {
            if (corpusTokens.find(t) != corpusTokens.end())
            {
                continue; // exact whole word
            }
            // Tab labels abbreviate ("act" for actions, "perf" for performance): a token also
            // matches as a PREFIX of a corpus word. One direction only — a tab token LONGER than
            // the corpus word ("resumes" vs "resume") stays a miss.
            bool prefixHit = false;
            for (const auto& w : corpusTokens)
            {
                if (w.size() > t.size() && w.compare(0, t.size(), t) == 0)
                {
                    prefixHit = true;
                    break;
                }
            }
            if (!prefixHit)
            {
                return 0; // all-or-nothing — a partial overlap on one generic word is noise
            }
        }
        return 50;
    }

    int PickClaudeTab(const std::vector<std::wstring>& tabNames, const std::vector<std::wstring>& titleHints, std::wstring_view cwdLeaf, const std::unordered_set<std::wstring>& headCorpusTokens, const std::unordered_set<std::wstring>& fullCorpusTokens, int& outScore, bool& outUnique)
    {
        // Token sets per tab, for the generic-prefix disqualifier (see the header): a tab whose
        // token set is a STRICT subset of a sibling's may not win on the token tier.
        std::vector<std::unordered_set<std::wstring>> tokenSets;
        tokenSets.reserve(tabNames.size());
        for (const auto& n : tabNames)
        {
            tokenSets.push_back(TokenizeTextLower(n));
        }
        const auto strictSubsetOfASibling = [&](size_t i) {
            const auto& a = tokenSets[i];
            if (a.empty())
            {
                return false;
            }
            for (size_t j = 0; j < tokenSets.size(); ++j)
            {
                const auto& b = tokenSets[j];
                if (j == i || b.size() <= a.size())
                {
                    continue; // equal sets stay eligible — a genuine tie is the unique-guard's job
                }
                bool subset = true;
                for (const auto& t : a)
                {
                    if (b.find(t) == b.end())
                    {
                        subset = false;
                        break;
                    }
                }
                if (subset)
                {
                    return true;
                }
            }
            return false;
        };

        int best = -1;
        outScore = 0;
        outUnique = true;
        for (size_t i = 0; i < tabNames.size(); ++i)
        {
            int s = 0;
            if (titleHints.empty())
            {
                s = ScoreClaudeTabName(tabNames[i], {}, cwdLeaf); // claude-word/glyph/cwd rules still apply
            }
            for (const auto& t : titleHints)
            {
                const int v = ScoreClaudeTabName(tabNames[i], t, cwdLeaf);
                s = v > s ? v : s;
            }
            if (!strictSubsetOfASibling(i))
            {
                // Head tier (50) over full tier (45): a purpose-named tab must outrank a tab that
                // merely matches a word from some later prompt.
                if (ScoreTabNameTokens(tabNames[i], headCorpusTokens) > 0)
                {
                    s = 50 > s ? 50 : s;
                }
                else if (ScoreTabNameTokens(tabNames[i], fullCorpusTokens) > 0)
                {
                    s = 45 > s ? 45 : s;
                }
            }
            if (s > outScore)
            {
                outScore = s;
                best = static_cast<int>(i);
                outUnique = true;
            }
            else if (s == outScore && s > 0)
            {
                outUnique = false;
            }
        }
        return best;
    }

    int ScoreClaudeTabName(std::wstring_view tabName, std::wstring_view titleHint, std::wstring_view cwdLeaf)
    {
        const auto trim = [](std::wstring_view s) {
            while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
            {
                s.remove_suffix(1);
            }
            return s;
        };
        const std::wstring name = LowerCopy(trim(tabName));
        if (name.empty())
        {
            return 0;
        }
        const std::wstring hint = LowerCopy(trim(titleHint));
        int score = 0;
        if (!hint.empty() && name == hint)
        {
            score = 100; // a tab named exactly the conversation title — the strongest signal
        }
        else if (name.find(L"claude") != std::wstring::npos)
        {
            score = 90; // the word itself — claude's own OSC title or a user rename
        }
        else if (name[0] == L'\x2733' || name[0] == L'\x2736' || name[0] == L'\x273D' || name[0] == L'\x2738')
        {
            score = 80; // claude's OSC status glyph leads the title it sets while working
        }
        if (score < 70)
        {
            // Full containment either way (a tab named with the title plus decoration, or with a
            // truncated title). The CONTAINED side must carry >= 6 chars of signal.
            if ((hint.size() >= 6 && name.find(hint) != std::wstring::npos) ||
                (name.size() >= 6 && !hint.empty() && hint.find(name) != std::wstring::npos))
            {
                score = 70;
            }
        }
        if (score < 60)
        {
            // The hint's HEAD, capped at 16 chars (so a name that is a strict prefix of the hint —
            // or glyph-prefixed — still hits) and at least 8 (no noise).
            const size_t cap = hint.size() < 16 ? hint.size() : 16;
            if (cap >= 8 && name.find(hint.substr(0, cap)) != std::wstring::npos)
            {
                score = 60;
            }
        }
        if (score < 40 && !cwdLeaf.empty())
        {
            // Weakest: the working-dir leaf (shells commonly title tabs by cwd).
            if (name.find(LowerCopy(cwdLeaf)) != std::wstring::npos)
            {
                score = 40;
            }
        }
        return score;
    }

    bool BringClaudeWindowToFront(uint32_t claudePid, uint32_t hostShellPid, std::wstring_view sessionId, std::wstring_view titleHint, std::wstring_view cwd)
    {
        // Everything a tab can be matched against. The caller's display title is one hint; the
        // transcript (one head read) contributes the user-set custom title + the first prompt as
        // further hints, and the conversation corpus (title + human prompts, tokenized) for the
        // hand-renamed-tab overlap tier. Duplicated hints are harmless (scores are max-combined).
        TabPickHints hints;
        hints.cwdLeaf = PathLeaf(cwd);
        if (!titleHint.empty())
        {
            hints.titles.emplace_back(titleHint);
        }
        if (!sessionId.empty())
        {
            // 2 MB head: human prompts are sparse among assistant/tool lines, and a 256 KB read
            // proved too shallow on real transcripts (one giant first turn swallowed it — corpus
            // of 1 prompt). One-shot on a click, off the UI thread — the depth is affordable.
            const auto ti = ReadTranscriptInfo(cwd, sessionId, 2 * 1024 * 1024, 200);
            if (ti.found)
            {
                if (!ti.customTitle.empty())
                {
                    hints.titles.push_back(ti.customTitle);
                }
                if (!ti.title.empty())
                {
                    hints.titles.push_back(ti.title);
                }
                // Head corpus = the conversation's PURPOSE (title + custom title + first prompt);
                // full corpus = every prompt read. PickClaudeTab ranks head hits above full hits.
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
                hints.headTokens = TokenizeTextLower(head);
                hints.fullTokens = TokenizeTextLower(full);
            }
        }

        // COM for the UIA client. MTA per UIA client guidance — we are on a worker thread, never
        // the UI one. RPC_E_CHANGED_MODE (already initialized STA here) is still usable; it is
        // just not ours to uninitialize.
        const HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        const bool ownCom = SUCCEEDED(coInit);

        // Inner scope so every ComPtr (uia + cached tab elements) releases BEFORE CoUninitialize.
        const bool ok = [&]() -> bool {
            Microsoft::WRL::ComPtr<IUIAutomation> uia; // created lazily, only if a tab strip needs reading
            const auto ensureUia = [&]() -> IUIAutomation* {
                if (!uia)
                {
                    ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
                }
                return uia.Get();
            };

            const auto snap = SnapshotProcesses();
            std::unordered_map<uint32_t, const ProcEntry*> byPid;
            byPid.reserve(snap.size());
            for (const auto& e : snap)
            {
                byPid[e.pid] = &e;
            }

            // 1) Ancestor chain, nearest-first: claude -> host shell -> ... -> the terminal/editor
            //    that owns a visible window (WindowsTerminal.exe / ConEmu / Code.exe / ...).
            //    Bounded + cycle-guarded (pids recycle); stops at explorer/system images.
            //    hostShellPid roots the walk when the claude itself already exited (a dead pid is
            //    simply absent from the snapshot — never a stale re-used one followed blindly).
            std::vector<uint32_t> chain;
            {
                uint32_t cur = byPid.count(claudePid) != 0 ? claudePid :
                                                             (byPid.count(hostShellPid) != 0 ? hostShellPid : 0);
                for (int depth = 0; cur != 0 && depth < 16; ++depth)
                {
                    const auto it = byPid.find(cur);
                    if (it == byPid.end() || IsAncestorBoundaryImage(it->second->image))
                    {
                        break;
                    }
                    chain.push_back(cur);
                    const uint32_t parent = it->second->ppid;
                    if (parent == cur)
                    {
                        break;
                    }
                    cur = parent;
                }
            }

            HWND target = nullptr;
            int tabIndex = -1; // the claude's tab in targetTabs, when already resolved
            bool tabUnique = false; // selection requires a UNIQUE best (never flip to a guessed tab)
            std::vector<UiaTab> targetTabs;

            for (const auto pid : chain)
            {
                auto wins = TopLevelWindowsOfPid(pid);
                if (wins.empty())
                {
                    // Classic console: the visible console window belongs to a conhost.exe CHILD
                    // of the shell. (A headless ConPTY host — OpenConsole, or a conhost that
                    // delegated to the default terminal — owns no visible window and falls through.)
                    for (const auto child : ChildrenOf(snap, pid))
                    {
                        const auto cit = byPid.find(child);
                        if (cit != byPid.end() && (ImageNameEq(cit->second->image, L"conhost.exe") || ImageNameEq(cit->second->image, L"openconsole.exe")))
                        {
                            wins = TopLevelWindowsOfPid(child);
                            if (!wins.empty())
                            {
                                break;
                            }
                        }
                    }
                }
                if (wins.empty())
                {
                    continue;
                }
                target = wins.front(); // topmost in z-order
                if (wins.size() > 1)
                {
                    // One process, several windows (a real WT hosts N windows in ONE
                    // WindowsTerminal.exe): pick the window whose tab strip actually matches the
                    // claude; the topmost stays the fallback when nothing scores.
                    int bestScore = 0;
                    for (const auto h : wins)
                    {
                        if (!IsTerminalIslandWindow(h))
                        {
                            continue;
                        }
                        auto tabs = UiaTabsOf(ensureUia(), h);
                        int score = 0;
                        bool unique = true;
                        const int idx = BestClaudeTabIn(tabs, hints, score, unique);
                        if (idx >= 0 && score > bestScore)
                        {
                            bestScore = score;
                            target = h;
                            tabIndex = idx;
                            tabUnique = unique;
                            targetTabs = std::move(tabs);
                        }
                    }
                }
                break; // nearest ancestor with a visible window wins
            }

            // 2) The process tree owns no visible window: a Win11 DEFAULT-TERMINAL handoff console
            //    (the chain's conhost delegated the session; the visible window is a Windows
            //    Terminal in an UNRELATED process). Best-effort: scan foreign WT-class windows for
            //    a CONFIDENT tab match — claude word / glyph / title summary, never the cwd leaf
            //    alone (too generic to gamble a foreground on).
            if (target == nullptr)
            {
                int bestScore = 59;
                for (const auto h : ForeignTerminalIslandWindows())
                {
                    auto tabs = UiaTabsOf(ensureUia(), h);
                    int score = 0;
                    bool unique = true;
                    const int idx = BestClaudeTabIn(tabs, hints, score, unique);
                    if (idx >= 0 && score > bestScore)
                    {
                        bestScore = score;
                        target = h;
                        tabIndex = idx;
                        tabUnique = unique;
                        targetTabs = std::move(tabs);
                    }
                }
            }

            if (target == nullptr)
            {
                return false;
            }

            RestoreAndForeground(target);

            // 3) A WT-class host also gets the claude's TAB selected (the window may host many).
            //    Probe now if the single-window path skipped it; select only on a real signal and
            //    only when there IS another tab to switch from — never guess.
            if (IsTerminalIslandWindow(target))
            {
                if (targetTabs.empty())
                {
                    targetTabs = UiaTabsOf(ensureUia(), target);
                    int score = 0;
                    tabIndex = BestClaudeTabIn(targetTabs, hints, score, tabUnique);
                    if (score <= 0)
                    {
                        tabIndex = -1;
                    }
                }
                if (tabIndex >= 0 && tabUnique && targetTabs.size() > 1)
                {
                    UiaSelectTab(targetTabs[static_cast<size_t>(tabIndex)]);
                }
            }
            return true;
        }();

        if (ownCom)
        {
            ::CoUninitialize();
        }
        return ok;
    }
}
