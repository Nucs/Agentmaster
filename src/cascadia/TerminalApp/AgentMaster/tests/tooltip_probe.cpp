// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster - ad-hoc diagnostic for the TAB TOOLTIP's wheel scrolling (NOT part of the test suite).
//
// WHY THIS EXISTS. The rich tab tooltip's body is wheel-scrollable, and the wheel is read on the tab
// header (doc/agentmaster/HANDOVER_tab-tooltip.md 4b-bis). hooks.log proves the notch REACHES us
// ("[tooltip-wheel] item: cb=1 tip=1 open=0") but that our gate sees the tooltip as CLOSED. Two very
// different bugs produce that identical line:
//
//   H1  the card IS on screen and ToolTip::IsOpen() is simply not a truthful gate for a
//       ToolTipService-OPENED (automatic) tooltip -> the fix is to stop gating on it.
//   H2  the framework DISMISSES the automatic tooltip on wheel input, before our handler runs
//       -> the card is genuinely gone, and no amount of gating/wiring can scroll it; the whole
//          "scroll a framework-owned ToolTip" approach is structurally dead.
//
// From inside the app the two are indistinguishable (we only have IsOpen, the very thing in doubt).
// From OUTSIDE they should be distinguishable by watching the UI Automation tree for a ToolTip element
// and correlating its appear/disappear timeline against hooks.log's [tooltip-wheel] timestamps (same
// [HH:MM:SS.mmm] local-time format, deliberately).
//
// OUTCOME (2026-07-27): BOTH hypotheses were wrong, and the answer came from the framework's own source
// rather than from this probe - see doc/agentmaster/HANDOVER_tab-tooltip.md "The attach-timing root
// cause". ToolTipService subscribes to the owner's PointerEntered inside RegisterToolTip, i.e. when the
// tooltip is ATTACHED; a tooltip attached mid-dwell never opens for that dwell, and our Unloaded
// handler detaches on every MUX container recycle. IsOpen was telling the truth all along.
//
// WHAT THIS PROBE IS GOOD FOR (and its traps, all of which bit during that investigation):
//   * watch mode (default): you hover + scroll, it prints a tooltip timeline. Read-only, no mouse.
//   * drive mode: it hovers a tab and sends one wheel notch itself. It BORROWS THE CURSOR (~6s) and
//     puts it back.
//   * "control" mode: hovers a button whose tooltip certainly exists - run this FIRST, because a
//     negative result from an unvalidated detector means nothing.
//   TRAPS: UIA hands out screen rects for tabs that are scrolled out, on another monitor, or covered by
//   another app - hovering those hovers SOMEONE ELSE'S window (hence the occlusion check); an app can
//   have several windows and the user's hovers happen in the FOCUSED one; and a full UIA sweep of a
//   terminal window costs ~1s, so "poll every 100ms" is really "poll every second".
//
// Build + run: _run-tooltip-probe.bat [seconds|drive] [exe-substring|control]
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    std::wstring Stamp()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t b[32]{};
        swprintf_s(b, L"[%02d:%02d:%02d.%03d]", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return b;
    }

    struct WinInfo
    {
        HWND hwnd{};
        DWORD pid{};
        std::wstring exe;
    };

    std::vector<WinInfo> g_windows;

    BOOL CALLBACK EnumProc(HWND hwnd, LPARAM)
    {
        if (!IsWindowVisible(hwnd))
        {
            return TRUE;
        }
        wchar_t cls[256]{};
        GetClassNameW(hwnd, cls, 255);
        // WT (and our fork) host their XAML island in a CASCADIA_HOSTING_WINDOW_CLASS top-level window.
        if (!wcsstr(cls, L"CASCADIA"))
        {
            return TRUE;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring exe;
        if (const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            wchar_t path[MAX_PATH]{};
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n))
            {
                exe = path;
            }
            CloseHandle(h);
        }
        g_windows.push_back({ hwnd, pid, exe });
        return TRUE;
    }

    // Depth- and budget-limited CONTROL-view walk looking for ToolTip elements. Bounded on purpose: a
    // terminal window's UIA subtree is enormous (every text range), and this polls a few times a second.
    void FindToolTips(IUIAutomation* uia, IUIAutomationTreeWalker* walker, IUIAutomationElement* el, int depth, int& budget, std::vector<ComPtr<IUIAutomationElement>>& out)
    {
        if (!el || depth > 6 || budget <= 0)
        {
            return;
        }
        --budget;
        CONTROLTYPEID ct = 0;
        el->get_CurrentControlType(&ct);
        if (ct == UIA_ToolTipControlTypeId)
        {
            out.emplace_back(el);
            return; // don't descend into the card itself
        }
        ComPtr<IUIAutomationElement> child;
        if (FAILED(walker->GetFirstChildElement(el, &child)) || !child)
        {
            return;
        }
        while (child && budget > 0)
        {
            FindToolTips(uia, walker, child.Get(), depth + 1, budget, out);
            ComPtr<IUIAutomationElement> next;
            if (FAILED(walker->GetNextSiblingElement(child.Get(), &next)))
            {
                break;
            }
            child = next;
        }
    }

    // A short, human-identifiable digest of a tooltip: its own Name plus the first few descendant
    // texts. This is what tells the RICH card (title / folder-branch / "claude . model" / numbered
    // prompts) apart from the plain title+keychord tooltip a non-managed tab shows.
    std::wstring DescribeToolTip(IUIAutomationTreeWalker* walker, IUIAutomationElement* tip)
    {
        std::wstring desc;
        BSTR name{};
        if (SUCCEEDED(tip->get_CurrentName(&name)) && name)
        {
            desc = name;
            SysFreeString(name);
        }
        // one level of descendants is enough to fingerprint the card
        std::vector<IUIAutomationElement*> stack;
        ComPtr<IUIAutomationElement> child;
        int texts = 0;
        if (SUCCEEDED(walker->GetFirstChildElement(tip, &child)) && child)
        {
            while (child && texts < 6)
            {
                BSTR n2{};
                if (SUCCEEDED(child->get_CurrentName(&n2)) && n2)
                {
                    if (SysStringLen(n2) > 0)
                    {
                        if (!desc.empty())
                        {
                            desc += L" | ";
                        }
                        desc += n2;
                        ++texts;
                    }
                    SysFreeString(n2);
                }
                ComPtr<IUIAutomationElement> next;
                if (FAILED(walker->GetNextSiblingElement(child.Get(), &next)))
                {
                    break;
                }
                child = next;
            }
        }
        if (desc.size() > 160)
        {
            desc.resize(160);
            desc += L"...";
        }
        for (auto& c : desc)
        {
            if (c == L'\r' || c == L'\n')
            {
                c = L' ';
            }
        }
        return desc;
    }
}

namespace
{
    // Collect the window's UIA TabItems (name + rect) - the drive mode needs one to hover.
    struct TabInfo
    {
        std::wstring name;
        RECT rect{};
    };

    void CollectTabs(IUIAutomationTreeWalker* walker, IUIAutomationElement* el, int depth, int& budget, std::vector<TabInfo>& out, CONTROLTYPEID want = UIA_TabItemControlTypeId)
    {
        if (!el || depth > 8 || budget <= 0)
        {
            return;
        }
        --budget;
        CONTROLTYPEID ct = 0;
        el->get_CurrentControlType(&ct);
        if (ct == want)
        {
            TabInfo ti;
            BSTR n{};
            if (SUCCEEDED(el->get_CurrentName(&n)) && n)
            {
                ti.name = n;
                SysFreeString(n);
            }
            el->get_CurrentBoundingRectangle(&ti.rect);
            out.push_back(ti);
            return;
        }
        ComPtr<IUIAutomationElement> child;
        if (FAILED(walker->GetFirstChildElement(el, &child)) || !child)
        {
            return;
        }
        while (child && budget > 0)
        {
            CollectTabs(walker, child.Get(), depth + 1, budget, out, want);
            ComPtr<IUIAutomationElement> next;
            if (FAILED(walker->GetNextSiblingElement(child.Get(), &next)))
            {
                break;
            }
            child = next;
        }
    }

    // One poll: is a (visible) ToolTip anywhere in these windows' trees? Returns its digest, or "".
    // `only` restricts the sweep to ONE window - a full walk of a terminal window's UIA subtree costs
    // ~1s (every text range is an element), which is far too slow to time anything.
    std::wstring PollToolTip(IUIAutomation* uia, IUIAutomationTreeWalker* walker, HWND only = nullptr)
    {
        std::wstring key;
        for (const auto& w : g_windows)
        {
            if (only && w.hwnd != only)
            {
                continue;
            }
            if (!IsWindow(w.hwnd))
            {
                continue;
            }
            ComPtr<IUIAutomationElement> root;
            if (FAILED(uia->ElementFromHandle(w.hwnd, &root)) || !root)
            {
                continue;
            }
            std::vector<ComPtr<IUIAutomationElement>> tips;
            int budget = 600;
            FindToolTips(uia, walker, root.Get(), 0, budget, tips);
            for (const auto& tip : tips)
            {
                BOOL offscreen = FALSE;
                tip->get_CurrentIsOffscreen(&offscreen);
                if (offscreen)
                {
                    continue;
                }
                RECT r{};
                tip->get_CurrentBoundingRectangle(&r);
                wchar_t buf[512]{};
                swprintf_s(buf, L"pid=%lu rect=%ldx%ld@%ld,%ld \"%ls\"", w.pid, r.right - r.left, r.bottom - r.top, r.left, r.top, DescribeToolTip(walker, tip.Get()).c_str());
                if (!key.empty())
                {
                    key += L" ++ ";
                }
                key += buf;
            }
        }
        return key;
    }
}

namespace
{
    // (a) What is UNDER a screen point, and what is it nested in? A tooltip's card content answers here
    // even when a tree walk from the window element does not find it.
    void DumpPointChain(IUIAutomation* uia, IUIAutomationTreeWalker* walker, LONG x, LONG y, const wchar_t* what)
    {
        POINT pt{ x, y };
        ComPtr<IUIAutomationElement> el;
        if (FAILED(uia->ElementFromPoint(pt, &el)) || !el)
        {
            wprintf(L"%ls   [hit-test %ls @%ld,%ld] nothing\n", Stamp().c_str(), what, x, y);
            return;
        }
        wprintf(L"%ls   [hit-test %ls @%ld,%ld] ancestor chain:\n", Stamp().c_str(), what, x, y);
        ComPtr<IUIAutomationElement> cur = el;
        for (int i = 0; i < 6 && cur; ++i)
        {
            CONTROLTYPEID ct = 0;
            cur->get_CurrentControlType(&ct);
            BSTR n{};
            std::wstring name;
            if (SUCCEEDED(cur->get_CurrentName(&n)) && n)
            {
                name = n;
                SysFreeString(n);
            }
            if (name.size() > 70)
            {
                name.resize(70);
                name += L"...";
            }
            for (auto& c : name)
            {
                if (c == L'\r' || c == L'\n')
                {
                    c = L' ';
                }
            }
            RECT r{};
            cur->get_CurrentBoundingRectangle(&r);
            wprintf(L"        %d: type=%d%ls rect=%ldx%ld \"%ls\"\n", i, (int)ct,
                    (ct == UIA_ToolTipControlTypeId) ? L" <== TOOLTIP" : L"",
                    r.right - r.left, r.bottom - r.top, name.c_str());
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(cur.Get(), &parent)))
            {
                break;
            }
            cur = parent;
        }
    }

    std::vector<HWND> g_preWindows;
    BOOL CALLBACK EnumAllProc(HWND hwnd, LPARAM lp)
    {
        auto* v = reinterpret_cast<std::vector<HWND>*>(lp);
        v->push_back(hwnd);
        return TRUE;
    }

    // (b) A WINDOWED popup (XAML Islands hosts popups that escape the island in their own HWND) shows
    // up as a brand-new top-level window while the card is up.
    void DumpNewWindows(const wchar_t* what)
    {
        std::vector<HWND> now;
        EnumWindows(EnumAllProc, reinterpret_cast<LPARAM>(&now));
        int printed = 0;
        for (HWND h : now)
        {
            if (std::find(g_preWindows.begin(), g_preWindows.end(), h) != g_preWindows.end())
            {
                continue;
            }
            wchar_t cls[256]{};
            GetClassNameW(h, cls, 255);
            RECT r{};
            GetWindowRect(h, &r);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            wprintf(L"%ls   [new window %ls] class=%ls pid=%lu rect=%ldx%ld@%ld,%ld visible=%d\n",
                    Stamp().c_str(), what, cls, pid, r.right - r.left, r.bottom - r.top, r.left, r.top, IsWindowVisible(h) ? 1 : 0);
            ++printed;
        }
        if (!printed)
        {
            wprintf(L"%ls   [new window %ls] none\n", Stamp().c_str(), what);
        }
    }
}

// DRIVE mode: perform the experiment ourselves - hover a tab, wait for the card, send ONE wheel notch,
// and report whether the card survived. Borrows the cursor for ~6s and puts it back.
static int DriveExperiment(IUIAutomation* uia, IUIAutomationTreeWalker* walker, const std::wstring& exeFilter, bool controlOnly)
{
    // Prefer the FOREGROUND window when it matches: a background window may not raise hover at all,
    // and an app can have several windows (this bit us - the probe hovered a background one while the
    // user's real hovers happen in the focused one).
    const WinInfo* target = nullptr;
    WinInfo fg{};
    if (const HWND h = GetForegroundWindow())
    {
        wchar_t cls[256]{};
        GetClassNameW(h, cls, 255);
        if (wcsstr(cls, L"CASCADIA"))
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            std::wstring exe;
            if (const HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
            {
                wchar_t path[MAX_PATH]{};
                DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(ph, 0, path, &n))
                {
                    exe = path;
                }
                CloseHandle(ph);
            }
            if (exeFilter.empty() || exe.find(exeFilter) != std::wstring::npos)
            {
                fg = { h, pid, exe };
                target = &fg;
                if (std::find_if(g_windows.begin(), g_windows.end(), [h](const WinInfo& w) { return w.hwnd == h; }) == g_windows.end())
                {
                    g_windows.push_back(fg); // so the UIA sweep covers it too
                }
                wprintf(L"%ls using the FOREGROUND window\n", Stamp().c_str());
            }
        }
    }
    for (const auto& w : g_windows)
    {
        if (target)
        {
            break;
        }
        if (!exeFilter.empty() && w.exe.find(exeFilter) == std::wstring::npos)
        {
            continue;
        }
        if (IsIconic(w.hwnd))
        {
            continue; // a minimized window can't be hovered
        }
        target = &w;
        break;
    }
    if (!target)
    {
        wprintf(L"[error] no non-minimized window matching \"%ls\"\n", exeFilter.c_str());
        return 1;
    }
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(target->hwnd, &root)) || !root)
    {
        wprintf(L"[error] ElementFromHandle failed\n");
        return 1;
    }
    std::vector<TabInfo> tabs;
    int budget = 4000;
    CollectTabs(walker, root.Get(), 0, budget, tabs);

    // POSITIVE CONTROL. Before believing "no tooltip appeared", prove the detectors can see a tooltip
    // that certainly exists: the tab strip's "+" (new tab) button carries a plain WT tooltip. If THAT
    // one is invisible to us too, the instrument is broken and no negative result here means anything.
    if (controlOnly)
    {
        std::vector<TabInfo> buttons;
        int bbudget = 4000;
        CollectTabs(walker, root.Get(), 0, bbudget, buttons, UIA_ButtonControlTypeId);
        const TabInfo* btn = nullptr;
        for (const auto& b : buttons)
        {
            wprintf(L"      button: \"%.50ls\" rect=%ldx%ld@%ld,%ld\n", b.name.c_str(), b.rect.right - b.rect.left, b.rect.bottom - b.rect.top, b.rect.left, b.rect.top);
            if (b.rect.right <= b.rect.left || btn)
            {
                continue;
            }
            const POINT c{ (b.rect.left + b.rect.right) / 2, (b.rect.top + b.rect.bottom) / 2 };
            const HWND under = WindowFromPoint(c);
            if ((under ? GetAncestor(under, GA_ROOT) : nullptr) != target->hwnd)
            {
                continue;
            }
            btn = &b;
        }
        if (!btn)
        {
            wprintf(L"[error] no reachable button to use as the control\n");
            return 1;
        }
        const POINT c{ (btn->rect.left + btn->rect.right) / 2, (btn->rect.top + btn->rect.bottom) / 2 };
        POINT sv{};
        GetCursorPos(&sv);
        wprintf(L"%ls CONTROL: hovering button \"%.40ls\" at %ld,%ld\n", Stamp().c_str(), btn->name.c_str(), c.x, c.y);
        g_preWindows.clear();
        EnumWindows(EnumAllProc, reinterpret_cast<LPARAM>(&g_preWindows));
        SetCursorPos(c.x, c.y);
        Sleep(60);
        SetCursorPos(c.x + 2, c.y + 1);
        Sleep(1800);
        DumpPointChain(uia, walker, c.x, c.y + 40, L"40px below the button");
        DumpNewWindows(L"during button hover");
        const auto ctl = PollToolTip(uia, walker, target->hwnd);
        wprintf(L"%ls CONTROL result: %ls\n", Stamp().c_str(), ctl.empty() ? L"NO tooltip seen -> THE DETECTORS ARE BLIND" : ctl.c_str());
        SetCursorPos(sv.x, sv.y);
        return 0;
    }
    wprintf(L"%ls target hwnd=0x%p pid=%lu tabs=%zu\n", Stamp().c_str(), (void*)target->hwnd, target->pid, tabs.size());
    // Prefer a tab that is NOT a plain shell and NOT the pinned Manager tab - only a managed claude
    // session carries the rich card, and the Manager tab carries none at all.
    const TabInfo* pick = nullptr;
    for (const auto& t : tabs)
    {
        wprintf(L"      tab: \"%.70ls\"%ls\n", t.name.c_str(), (t.rect.right > t.rect.left) ? L"" : L" (no rect)");
        if (t.rect.right <= t.rect.left)
        {
            continue;
        }
        const bool skip = t.name.find(L"PowerShell") != std::wstring::npos || t.name.find(L"Command Prompt") != std::wstring::npos ||
                          t.name.find(L"cmd") != std::wstring::npos || t.name.find(L"Agent Manager") != std::wstring::npos;
        if (skip || pick)
        {
            continue;
        }
        // Must be REACHABLE by the pointer: UIA hands out screen rects for tabs that are scrolled out
        // of the strip, on another monitor, or simply covered by another app's window - hovering those
        // hovers someone else's window and makes the whole experiment a lie.
        const POINT c{ (t.rect.left + t.rect.right) / 2, (t.rect.top + t.rect.bottom) / 2 };
        const HWND under = WindowFromPoint(c);
        if (const HWND top = under ? GetAncestor(under, GA_ROOT) : nullptr; top != target->hwnd)
        {
            wchar_t cls[128]{};
            if (top)
            {
                GetClassNameW(top, cls, 127);
            }
            wprintf(L"        (skipped - covered at %ld,%ld by %ls)\n", c.x, c.y, cls[0] ? cls : L"nothing");
            continue;
        }
        pick = &t;
    }
    if (!pick)
    {
        wprintf(L"[error] no usable tab found\n");
        return 1;
    }
    const POINT center{ (pick->rect.left + pick->rect.right) / 2, (pick->rect.top + pick->rect.bottom) / 2 };
    wprintf(L"%ls hovering tab \"%.60ls\" at %ld,%ld\n", Stamp().c_str(), pick->name.c_str(), center.x, center.y);

    g_preWindows.clear();
    EnumWindows(EnumAllProc, reinterpret_cast<LPARAM>(&g_preWindows)); // baseline for the windowed-popup diff

    POINT saved{};
    GetCursorPos(&saved);
    SetCursorPos(center.x, center.y);
    // nudge: a hover needs pointer MOVEMENT to register, not just a teleport
    Sleep(60);
    SetCursorPos(center.x + 2, center.y + 1);
    Sleep(60);
    SetCursorPos(center.x, center.y);

    // OCCLUSION CHECK - the single most important sanity check in this whole probe. UIA gives the tab's
    // SCREEN rect, but if another window covers that point we are hovering THAT window, and a "no
    // tooltip appeared" result would be meaningless (it is what invalidated the first runs of this).
    {
        const HWND under = WindowFromPoint(center);
        const HWND underTop = under ? GetAncestor(under, GA_ROOT) : nullptr;
        wchar_t cls[256]{};
        DWORD upid = 0;
        if (underTop)
        {
            GetClassNameW(underTop, cls, 255);
            GetWindowThreadProcessId(underTop, &upid);
        }
        const bool ok = (underTop == target->hwnd);
        wprintf(L"%ls   [occlusion] point owned by hwnd=0x%p pid=%lu class=%ls  -> %ls\n",
                Stamp().c_str(), (void*)underTop, upid, cls,
                ok ? L"OK (the target)" : L"*** NOT THE TARGET - the window is covered; result is meaningless ***");
        if (!ok)
        {
            SetCursorPos(saved.x, saved.y);
            return 1;
        }
    }

    // A blind wait for the system hover delay, then ONE sweep. Polling in a loop is not an option: a
    // single sweep of this window's UIA tree costs about a second, so a "poll every 100ms" loop is
    // really a poll-every-second loop that pins the user's cursor for a minute.
    Sleep(1600);

    // Where does the card actually live? Two independent detectors, because walking DOWN from the
    // window element missed even the Store terminal's plain tab tooltip - under XAML Islands a popup
    // is NOT necessarily a shallow descendant of the window's UIA element.
    //   (a) HIT-TEST the point where the card renders (just below the tab) and walk UP for a ToolTip.
    //   (b) diff the top-level window list - a WINDOWED popup shows up as its own HWND.
    DumpPointChain(uia, walker, center.x, center.y + 45, L"45px below the tab");
    DumpNewWindows(L"during hover");

    std::wstring shown = PollToolTip(uia, walker, target->hwnd);
    if (shown.empty())
    {
        wprintf(L"%ls NO tooltip in the UIA tree ~1.6s into the hover (one sweep)\n", Stamp().c_str());
        Sleep(1500);
        shown = PollToolTip(uia, walker, target->hwnd); // second chance, further into the dwell
        if (shown.empty())
        {
            wprintf(L"%ls NO tooltip on the second sweep either\n", Stamp().c_str());
            SetCursorPos(saved.x, saved.y);
            return 0;
        }
    }
    wprintf(L"%ls   ++ tooltip SHOWN  %ls\n", Stamp().c_str(), shown.c_str());

    // ONE wheel notch, exactly where the pointer already is (over the tab header).
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    wprintf(L"%ls   >> sending ONE wheel notch (-120)\n", Stamp().c_str());
    SendInput(1, &in, sizeof(in));

    const auto after = PollToolTip(uia, walker, target->hwnd);
    SetCursorPos(saved.x, saved.y);
    if (after.empty())
    {
        wprintf(L"%ls   -- tooltip GONE after the notch\n", Stamp().c_str());
        wprintf(L"\nVERDICT: the wheel DISMISSES the card -> scrolling a framework tooltip is structurally dead.\n");
        return 0;
    }
    if (after != shown)
    {
        wprintf(L"%ls   ~~ tooltip CHANGED after the notch: %ls\n", Stamp().c_str(), after.c_str());
    }
    wprintf(L"\nVERDICT: the card SURVIVED the notch (still in the UIA tree 1.2s later).\n"
            L"  => the framework did NOT dismiss it. Cross-check hooks.log's [tooltip-wheel] line for\n"
            L"     this instant: open=0 there means ToolTip::IsOpen is not a truthful gate for the card\n"
            L"     we are actually showing (a stale/ replaced ToolTip object), not a dismissal.\n");
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    // Unbuffered: this probe is expected to be piped, and a crash mid-experiment must not swallow the
    // very lines that say how far it got.
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool drive = argc > 1 && std::wstring{ argv[1] } == L"drive";
    const int seconds = (argc > 1 && !drive) ? _wtoi(argv[1]) : 25;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        wprintf(L"[error] CoInitializeEx failed\n");
        return 2;
    }
    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))) || !uia)
    {
        wprintf(L"[error] could not create UIAutomation\n");
        return 2;
    }
    ComPtr<IUIAutomationTreeWalker> walker;
    uia->get_ControlViewWalker(&walker);
    if (!walker)
    {
        wprintf(L"[error] no control view walker\n");
        return 2;
    }

    EnumWindows(EnumProc, 0);
    if (g_windows.empty())
    {
        wprintf(L"[error] no CASCADIA_HOSTING_WINDOW_CLASS windows found (is the app running?)\n");
        return 1;
    }
    wprintf(L"%ls watching %zu terminal window(s) for %ds:\n", Stamp().c_str(), g_windows.size(), seconds);
    for (const auto& w : g_windows)
    {
        wprintf(L"    hwnd=0x%p pid=%lu  %ls\n", (void*)w.hwnd, w.pid, w.exe.c_str());
    }
    if (drive)
    {
        // %2 = a substring of the exe path to target (default: the Debug/dev instance), or the literal
        // "control" to run the POSITIVE CONTROL (hover a button whose tooltip certainly exists).
        const bool controlOnly = (argc > 2 && std::wstring{ argv[2] } == L"control");
        const std::wstring filter = (argc > 2 && !controlOnly) ? argv[2] : L"\\bin\\x64\\Debug\\";
        const int rc = DriveExperiment(uia.Get(), walker.Get(), filter, controlOnly);
        CoUninitialize();
        return rc;
    }
    wprintf(L"\n  NOW: hover a MANAGED (claude) tab until the rich card appears, then spin the wheel\n"
            L"  while STAYING on the tab. Watch the timeline below, then compare its timestamps with\n"
            L"  the [tooltip-wheel] lines in hooks.log.\n\n");

    // Poll. A tooltip is identified by (hwnd + its digest) so a card whose CONTENT is swapped in place
    // reads as a change, not as a close+open.
    std::wstring lastKey;
    const auto tEnd = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    int polls = 0, seen = 0;
    while (GetTickCount64() < tEnd)
    {
        std::wstring key, detail;
        for (const auto& w : g_windows)
        {
            if (!IsWindow(w.hwnd))
            {
                continue;
            }
            ComPtr<IUIAutomationElement> root;
            if (FAILED(uia->ElementFromHandle(w.hwnd, &root)) || !root)
            {
                continue;
            }
            std::vector<ComPtr<IUIAutomationElement>> tips;
            int budget = 600;
            FindToolTips(uia.Get(), walker.Get(), root.Get(), 0, budget, tips);
            for (const auto& tip : tips)
            {
                BOOL offscreen = FALSE;
                tip->get_CurrentIsOffscreen(&offscreen);
                if (offscreen)
                {
                    continue; // parked/recycled peer, not a visible card
                }
                RECT r{};
                tip->get_CurrentBoundingRectangle(&r);
                const auto d = DescribeToolTip(walker.Get(), tip.Get());
                wchar_t buf[512]{};
                swprintf_s(buf, L"pid=%lu rect=%ldx%ld@%ld,%ld \"%ls\"", w.pid, r.right - r.left, r.bottom - r.top, r.left, r.top, d.c_str());
                if (!key.empty())
                {
                    key += L" ++ ";
                }
                key += buf;
            }
        }
        ++polls;
        if (key != lastKey)
        {
            if (key.empty())
            {
                wprintf(L"%ls   -- tooltip GONE\n", Stamp().c_str());
            }
            else
            {
                ++seen;
                wprintf(L"%ls   ++ tooltip SHOWN  %ls\n", Stamp().c_str(), key.c_str());
            }
            fflush(stdout);
            lastKey = key;
        }
        Sleep(90);
    }
    wprintf(L"\n%ls done (%d polls, %d appearance(s)).\n", Stamp().c_str(), polls, seen);
    wprintf(L"  Now: grep '\\[tooltip-wheel\\]' <profile>\\hooks.log and line the timestamps up.\n"
            L"    tooltip still SHOWN while the log says open=0  => IsOpen is not a truthful gate (H1)\n"
            L"    tooltip GONE at/just before the wheel line      => the framework dismisses on wheel (H2)\n");
    CoUninitialize();
    return 0;
}
