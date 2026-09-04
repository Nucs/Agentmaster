// Agentmaster: LIVE read-only profiler for a running instance (no dump, no kill).
//   census <pid>                 - every thread: tid, creation, cumulative CPU, ~3s CPU delta,
//                                  Win32 start address symbolized; UI threads (window owners) marked;
//                                  start-address histogram (thread-leak classifier).
//   sample <pid> <tid> <n> <ms>  - RIP-sample a thread n times every ms; symbol histogram +
//                                  a few full StackWalkEx stacks (symbolized, PDBs from imageDir).
// Symbols: pass the Release layout dir as the sym search path via env LIVESAMPLE_SYMDIR
// (defaults to the CascadiaPackage Release layout). Suspends are microseconds; strictly read-only.
#define _AMD64_
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <psapi.h>
#include <winternl.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

typedef NTSTATUS(NTAPI* PFN_NtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static HANDLE g_proc = nullptr;

static std::wstring symAt(ULONG64 pc)
{
    alignas(16) BYTE buf[sizeof(SYMBOL_INFOW) + 1024 * sizeof(wchar_t)] = {};
    auto* si = (SYMBOL_INFOW*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFOW);
    si->MaxNameLen = 1024;
    DWORD64 disp = 0;
    wchar_t out[1400];
    // module name
    wchar_t modName[MAX_PATH] = L"?";
    HMODULE hm = nullptr;
    DWORD64 base = SymGetModuleBase64(g_proc, pc);
    if (base)
    {
        IMAGEHLP_MODULEW64 im{};
        im.SizeOfStruct = sizeof(im);
        if (SymGetModuleInfoW64(g_proc, pc, &im))
            wcscpy_s(modName, im.ModuleName);
    }
    if (SymFromAddrW(g_proc, pc, &disp, si))
        swprintf_s(out, L"%s!%s+0x%llx", modName, si->Name, disp);
    else if (base)
        swprintf_s(out, L"%s+0x%llx", modName, pc - base);
    else
        swprintf_s(out, L"0x%llx", pc);
    return out;
}

struct WndInfo { DWORD tid; std::wstring title; };
static std::vector<WndInfo> g_wnds;
static DWORD g_pid = 0;
static BOOL CALLBACK enumWndCb(HWND h, LPARAM)
{
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid)
    {
        wchar_t t[128] = L"";
        GetWindowTextW(h, t, 127);
        if (IsWindowVisible(h) || t[0])
            g_wnds.push_back({ tid, t });
    }
    return TRUE;
}

static double ftToSec(const FILETIME& ft)
{
    ULARGE_INTEGER u{ ft.dwLowDateTime, ft.dwHighDateTime };
    return u.QuadPart / 1e7;
}

static int census(DWORD pid)
{
    g_pid = pid;
    EnumWindows(enumWndCb, 0);

    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto NtQIT = (PFN_NtQueryInformationThread)GetProcAddress(ntdll, "NtQueryInformationThread");

    struct T
    {
        DWORD tid;
        HANDLE h;
        double cpu0, cpu1;
        FILETIME created;
        ULONG64 start;
    };
    std::vector<T> ts;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid)
            continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h)
            continue;
        FILETIME cr{}, ex{}, k{}, u{};
        GetThreadTimes(h, &cr, &ex, &k, &u);
        ULONG64 start = 0;
        if (NtQIT)
        {
            ULONG got = 0;
            NtQIT(h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), &got);
        }
        ts.push_back({ te.th32ThreadID, h, ftToSec(k) + ftToSec(u), 0, cr, start });
    }
    CloseHandle(snap);

    Sleep(3000);
    for (auto& t : ts)
    {
        FILETIME cr{}, ex{}, k{}, u{};
        GetThreadTimes(t.h, &cr, &ex, &k, &u);
        t.cpu1 = ftToSec(k) + ftToSec(u);
    }

    wprintf(L"threads=%zu  windows-owning-tids:", ts.size());
    {
        std::map<DWORD, int> uiTids;
        for (auto& w : g_wnds) uiTids[w.tid]++;
        for (auto& [tid, n] : uiTids) wprintf(L" %lu(x%d)", tid, n);
    }
    wprintf(L"\n\n--- top 25 by 3s CPU delta ---\n");
    std::sort(ts.begin(), ts.end(), [](const T& a, const T& b) { return (a.cpu1 - a.cpu0) > (b.cpu1 - b.cpu0); });
    for (size_t i = 0; i < ts.size() && i < 25; ++i)
    {
        auto& t = ts[i];
        SYSTEMTIME st{};
        FILETIME lf{};
        FileTimeToLocalFileTime(&t.created, &lf);
        FileTimeToSystemTime(&lf, &st);
        wprintf(L"tid=%-6lu delta=%6.1f%%  total=%9.1fs  created=%02d-%02d %02d:%02d:%02d  start=%s\n",
                t.tid, (t.cpu1 - t.cpu0) / 3.0 * 100.0, t.cpu1,
                st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                symAt(t.start).c_str());
    }

    wprintf(L"\n--- start-address histogram (all %zu threads) ---\n", ts.size());
    std::map<std::wstring, int> hist;
    for (auto& t : ts)
        hist[symAt(t.start)]++;
    std::vector<std::pair<std::wstring, int>> hv(hist.begin(), hist.end());
    std::sort(hv.begin(), hv.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [name, n] : hv)
        if (n >= 2)
            wprintf(L"%5d  %s\n", n, name.c_str());
    wprintf(L"(singletons: %d)\n", (int)std::count_if(hv.begin(), hv.end(), [](auto& p) { return p.second == 1; }));

    // creation-time histogram per hour for the biggest start-address bucket
    if (!hv.empty())
    {
        wprintf(L"\n--- creation times of the top bucket [%s] per hour ---\n", hv[0].first.c_str());
        std::map<std::wstring, int> byHour;
        for (auto& t : ts)
            if (symAt(t.start) == hv[0].first)
            {
                SYSTEMTIME st{};
                FILETIME lf{};
                FileTimeToLocalFileTime(&t.created, &lf);
                FileTimeToSystemTime(&lf, &st);
                wchar_t key[32];
                swprintf_s(key, L"%02d-%02d %02dh", st.wMonth, st.wDay, st.wHour);
                byHour[key]++;
            }
        for (auto& [k, n] : byHour)
            wprintf(L"  %s : %d\n", k.c_str(), n);
    }
    for (auto& t : ts)
        CloseHandle(t.h);
    return 0;
}

static int sample(DWORD pid, DWORD tid, int n, int periodMs)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h)
    {
        wprintf(L"OpenThread(%lu) failed gle=%lu\n", tid, GetLastError());
        return 1;
    }
    std::map<std::wstring, int> hist;
    int walksLeft = 6;
    for (int i = 0; i < n; ++i)
    {
        if (SuspendThread(h) == (DWORD)-1)
        {
            wprintf(L"SuspendThread failed gle=%lu\n", GetLastError());
            break;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        bool haveCtx = GetThreadContext(h, &ctx) != 0;
        std::wstring where;
        if (haveCtx)
            where = symAt(ctx.Rip);
        // every ~n/6-th sample: full walk while still suspended
        bool doWalk = haveCtx && walksLeft > 0 && (i % (n / 6 + 1) == 0);
        if (doWalk)
        {
            CONTEXT wctx{};
            wctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &wctx))
            {
                wprintf(L"\n=== stack @ sample %d (rip=%s) ===\n", i, where.c_str());
                STACKFRAME_EX fr{};
                fr.AddrPC.Offset = wctx.Rip; fr.AddrPC.Mode = AddrModeFlat;
                fr.AddrFrame.Offset = wctx.Rbp; fr.AddrFrame.Mode = AddrModeFlat;
                fr.AddrStack.Offset = wctx.Rsp; fr.AddrStack.Mode = AddrModeFlat;
                for (int f = 0; f < 48; ++f)
                {
                    if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, g_proc, h, &fr, &wctx, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr, SYM_STKWALK_DEFAULT))
                        break;
                    if (!fr.AddrPC.Offset)
                        break;
                    wprintf(L"  %02d %s\n", f, symAt(fr.AddrPC.Offset).c_str());
                }
                walksLeft--;
            }
        }
        ResumeThread(h);
        if (haveCtx)
            hist[where]++;
        Sleep(periodMs);
    }
    CloseHandle(h);
    wprintf(L"\n--- RIP histogram (%d samples) ---\n", n);
    std::vector<std::pair<std::wstring, int>> hv(hist.begin(), hist.end());
    std::sort(hv.begin(), hv.end(), [](auto& a, auto& b) { return a.second > b.second; });
    int shown = 0;
    for (auto& [name, cnt] : hv)
    {
        wprintf(L"%5.1f%%  %s\n", cnt * 100.0 / n, name.c_str());
        if (++shown >= 25)
            break;
    }
    return 0;
}

// Agentmaster: `walk` — an aggregating stack-walk profiler for ONE thread.
//   walk <pid> <tid> <n> <periodMs> [episodeMs]
// Every sample (not every n/6-th): suspend → CONTEXT_FULL → StackWalkEx up to 64 RAW pcs → resume,
// so the suspend window stays microseconds (symbolization happens once per UNIQUE pc at the end).
// A sample is IDLE only when its leaf is the message-pump wait (GetMessage / MsgWaitForMultipleObjects);
// a wait on a lock/condvar/handle is BUSY — it is a stall contributor the user feels.
// Reports: (a) inclusive histogram over OUR-module frames (each symbol once per busy sample),
// (b) the top collapsed busy stacks (our-module frames only, leaf first), (c) the exclusive leaf
// histogram, (d) BUSY EPISODES — runs of consecutive busy samples spanning >= episodeMs (default 500),
// each with its wall span, the dominant collapsed stack and its top leaves — the tab-switch stalls
// the 20s [ui-stall] watchdog never logs.
static bool isOurModule(const std::wstring& sym)
{
    return sym.rfind(L"TerminalApp!", 0) == 0 || sym.rfind(L"Agentmaster!", 0) == 0 ||
           sym.rfind(L"Microsoft.Terminal.Control!", 0) == 0 || sym.rfind(L"Microsoft.Terminal.Remoting!", 0) == 0 ||
           sym.rfind(L"TerminalConnection!", 0) == 0 || sym.rfind(L"Microsoft.Terminal.Settings.Model!", 0) == 0 ||
           sym.rfind(L"Microsoft.Terminal.Settings.Editor!", 0) == 0;
}

static bool isPumpIdleLeaf(const std::wstring& sym)
{
    return sym.find(L"NtUserGetMessage") != std::wstring::npos ||
           sym.find(L"NtUserMsgWaitForMultipleObjectsEx") != std::wstring::npos ||
           sym.find(L"NtUserPeekMessage") != std::wstring::npos;
}

// Strip the "+0x<disp>" tail so frames of one function collapse together.
static std::wstring symNoDisp(const std::wstring& s)
{
    auto p = s.rfind(L"+0x");
    return p == std::wstring::npos ? s : s.substr(0, p);
}

static int walk(DWORD pid, DWORD tid, int n, int periodMs, int episodeMs)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h)
    {
        wprintf(L"OpenThread(%lu) failed gle=%lu\n", tid, GetLastError());
        return 1;
    }
    struct Sample
    {
        double tSec; // wall offset from capture start
        std::vector<ULONG64> pcs; // leaf first
    };
    std::vector<Sample> samples;
    samples.reserve((size_t)n);
    LARGE_INTEGER qpf{}, q0{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&q0);
    FILETIME cr{}, ex{}, k0{}, u0{}, k1{}, u1{};
    GetThreadTimes(h, &cr, &ex, &k0, &u0);
    int suspendFails = 0;
    for (int i = 0; i < n; ++i)
    {
        if (SuspendThread(h) == (DWORD)-1)
        {
            ++suspendFails;
            Sleep(periodMs);
            continue;
        }
        Sample s{};
        LARGE_INTEGER q{};
        QueryPerformanceCounter(&q);
        s.tSec = double(q.QuadPart - q0.QuadPart) / double(qpf.QuadPart);
        CONTEXT wctx{};
        wctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &wctx))
        {
            STACKFRAME_EX fr{};
            fr.AddrPC.Offset = wctx.Rip; fr.AddrPC.Mode = AddrModeFlat;
            fr.AddrFrame.Offset = wctx.Rbp; fr.AddrFrame.Mode = AddrModeFlat;
            fr.AddrStack.Offset = wctx.Rsp; fr.AddrStack.Mode = AddrModeFlat;
            s.pcs.push_back(wctx.Rip);
            for (int f = 0; f < 64; ++f)
            {
                if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, g_proc, h, &fr, &wctx, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr, SYM_STKWALK_DEFAULT))
                    break;
                if (!fr.AddrPC.Offset)
                    break;
                if (f > 0 || fr.AddrPC.Offset != s.pcs.front())
                    s.pcs.push_back(fr.AddrPC.Offset);
            }
        }
        ResumeThread(h);
        samples.push_back(std::move(s));
        Sleep(periodMs);
    }
    GetThreadTimes(h, &cr, &ex, &k1, &u1);
    LARGE_INTEGER q1{};
    QueryPerformanceCounter(&q1);
    const double wall = double(q1.QuadPart - q0.QuadPart) / double(qpf.QuadPart);
    const double cpu = (ftToSec(k1) + ftToSec(u1)) - (ftToSec(k0) + ftToSec(u0));
    CloseHandle(h);

    // symbolize once per unique pc
    std::map<ULONG64, std::wstring> symCache;
    auto symOf = [&](ULONG64 pc) -> const std::wstring& {
        auto it = symCache.find(pc);
        if (it == symCache.end())
            it = symCache.emplace(pc, symAt(pc)).first;
        return it->second;
    };

    const size_t total = samples.size();
    size_t busy = 0;
    std::vector<char> isBusy(total, 0);
    std::map<std::wstring, int> leafHist;
    std::map<std::wstring, int> inclOurs;    // symbol -> busy samples containing it (once each)
    std::map<std::wstring, int> stackHist;   // collapsed our-frames signature -> count
    std::vector<std::wstring> sigOf(total);
    for (size_t i = 0; i < total; ++i)
    {
        auto& s = samples[i];
        if (s.pcs.empty())
            continue;
        const std::wstring leaf = symOf(s.pcs.front());
        leafHist[leaf]++;
        const bool b = !isPumpIdleLeaf(leaf);
        isBusy[i] = b ? 1 : 0;
        if (!b)
            continue;
        ++busy;
        std::map<std::wstring, int> seen;
        std::wstring sig;
        std::wstring lastNd; // StackWalkEx repeats a function across its inlined/FPO frames — fold consecutive dups
        int ourFrames = 0;
        for (auto pc : s.pcs)
        {
            const std::wstring& sym = symOf(pc);
            if (!isOurModule(sym))
                continue;
            const std::wstring nd = symNoDisp(sym);
            if (seen[nd]++ == 0)
                inclOurs[nd]++;
            if (ourFrames < 14 && nd != lastNd)
            {
                sig += nd;
                sig += L"\n";
                ++ourFrames;
                lastNd = nd;
            }
        }
        if (sig.empty())
        {
            // no frame of ours: signature by the top 3 foreign frames so the bucket is still readable
            for (size_t f = 0; f < s.pcs.size() && f < 3; ++f)
            {
                sig += symNoDisp(symOf(s.pcs[f]));
                sig += L"\n";
            }
            sig = L"[no frame of ours]\n" + sig;
        }
        sigOf[i] = sig;
        stackHist[sig]++;
    }

    wprintf(L"walk tid=%lu: %zu samples over %.1fs (period %dms, suspend-fails %d)  thread CPU in window = %.2fs (%.1f%% of a core)  busy samples = %zu (%.1f%%)\n",
            tid, total, wall, periodMs, suspendFails, cpu, wall > 0 ? cpu / wall * 100.0 : 0.0, busy, total ? busy * 100.0 / total : 0.0);

    wprintf(L"\n--- (a) INCLUSIVE our-frame histogram (%% of BUSY samples / %% of ALL samples), top 45 ---\n");
    {
        std::vector<std::pair<std::wstring, int>> v(inclOurs.begin(), inclOurs.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        int shown = 0;
        for (auto& [name, cnt] : v)
        {
            wprintf(L"%6.1f%% %6.1f%%  %s\n", busy ? cnt * 100.0 / busy : 0.0, total ? cnt * 100.0 / total : 0.0, name.c_str());
            if (++shown >= 45)
                break;
        }
    }

    wprintf(L"\n--- (b) top collapsed BUSY stacks (our-module frames, leaf first), top 14 ---\n");
    {
        std::vector<std::pair<std::wstring, int>> v(stackHist.begin(), stackHist.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        int shown = 0;
        for (auto& [sig, cnt] : v)
        {
            wprintf(L"\n[%d samples, %.1f%% of busy]\n", cnt, busy ? cnt * 100.0 / busy : 0.0);
            size_t pos = 0;
            while (pos < sig.size())
            {
                size_t nl = sig.find(L'\n', pos);
                if (nl == std::wstring::npos)
                    nl = sig.size();
                wprintf(L"    %s\n", sig.substr(pos, nl - pos).c_str());
                pos = nl + 1;
            }
            if (++shown >= 14)
                break;
        }
    }

    wprintf(L"\n--- (c) EXCLUSIVE leaf histogram (all samples), top 25 ---\n");
    {
        std::vector<std::pair<std::wstring, int>> v(leafHist.begin(), leafHist.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        int shown = 0;
        for (auto& [name, cnt] : v)
        {
            wprintf(L"%5.1f%%  %s\n", total ? cnt * 100.0 / total : 0.0, name.c_str());
            if (++shown >= 25)
                break;
        }
    }

    wprintf(L"\n--- (d) BUSY EPISODES >= %dms (consecutive busy samples), with dominant collapsed stack ---\n", episodeMs);
    {
        int episodes = 0;
        size_t i = 0;
        while (i < total)
        {
            if (!isBusy[i])
            {
                ++i;
                continue;
            }
            size_t j = i;
            while (j + 1 < total && isBusy[j + 1])
                ++j;
            const double span = samples[j].tSec - samples[i].tSec + periodMs / 1000.0;
            if (span * 1000.0 >= episodeMs)
            {
                ++episodes;
                std::map<std::wstring, int> sigs, leaves;
                for (size_t k = i; k <= j; ++k)
                {
                    sigs[sigOf[k]]++;
                    if (!samples[k].pcs.empty())
                        leaves[symNoDisp(symOf(samples[k].pcs.front()))]++;
                }
                std::vector<std::pair<std::wstring, int>> sv(sigs.begin(), sigs.end()), lv(leaves.begin(), leaves.end());
                std::sort(sv.begin(), sv.end(), [](auto& a, auto& b) { return a.second > b.second; });
                std::sort(lv.begin(), lv.end(), [](auto& a, auto& b) { return a.second > b.second; });
                wprintf(L"\n### episode #%d  t=+%.1fs .. +%.1fs  span=%.2fs  samples=%zu\n", episodes, samples[i].tSec, samples[j].tSec, span, j - i + 1);
                wprintf(L"    leaves:");
                for (size_t l = 0; l < lv.size() && l < 4; ++l)
                    wprintf(L"  %s (%d)", lv[l].first.c_str(), lv[l].second);
                wprintf(L"\n");
                for (size_t sIdx = 0; sIdx < sv.size() && sIdx < 2; ++sIdx)
                {
                    wprintf(L"    stack #%zu (%d of %zu samples):\n", sIdx + 1, sv[sIdx].second, j - i + 1);
                    const auto& sig = sv[sIdx].first;
                    size_t pos = 0;
                    while (pos < sig.size())
                    {
                        size_t nl = sig.find(L'\n', pos);
                        if (nl == std::wstring::npos)
                            nl = sig.size();
                        wprintf(L"        %s\n", sig.substr(pos, nl - pos).c_str());
                        pos = nl + 1;
                    }
                }
            }
            i = j + 1;
        }
        if (!episodes)
            wprintf(L"(none)\n");
    }
    return 0;
}

// Agentmaster: `lockwatch` — WHO holds the terminal lock the UI thread is waiting on?
//   lockwatch <pid> <uiTid> <n> <periodMs>
// Samples the UI thread; whenever it is caught inside til::recursive_ticket_lock::lock (its leaf a
// WaitOnAddress kernel wait), it stack-walks EVERY other thread of the process whose start address
// is a terminal render / ConPTY-output / std::thread worker (the only threads that ever take a
// ControlCore lock outside the UI thread) and records their collapsed stacks. The thread that shows
// up INSIDE Microsoft.Terminal.Control / TerminalCore / the renderer while the UI thread waits is the
// holder. Also reports how many UI samples were lock-waits and the longest contiguous wait.
static bool isLockWaitStack(const std::vector<ULONG64>& pcs, const std::function<const std::wstring&(ULONG64)>& symOf)
{
    if (pcs.empty())
        return false;
    for (size_t i = 0; i < pcs.size() && i < 8; ++i)
    {
        const std::wstring& s = symOf(pcs[i]);
        if (s.find(L"ticket_lock") != std::wstring::npos)
            return true;
    }
    return false;
}

static int lockwatch(DWORD pid, DWORD uiTid, int n, int periodMs)
{
    HANDLE hUi = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, uiTid);
    if (!hUi)
    {
        wprintf(L"OpenThread(%lu) failed gle=%lu\n", uiTid, GetLastError());
        return 1;
    }
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto NtQIT = (PFN_NtQueryInformationThread)GetProcAddress(ntdll, "NtQueryInformationThread");

    // enumerate candidate holder threads once (render / conpty output / std::thread pools); re-enumerated every 50 hits
    struct Cand
    {
        DWORD tid;
        HANDLE h;
        std::wstring startSym;
    };
    std::vector<Cand> cands;
    auto enumerate = [&]() {
        for (auto& c : cands)
            CloseHandle(c.h);
        cands.clear();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{ sizeof(te) };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == uiTid)
                continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            ULONG64 start = 0;
            ULONG got = 0;
            if (NtQIT)
                NtQIT(h, 9, &start, sizeof(start), &got);
            const std::wstring ss = symAt(start);
            if (ss.find(L"nvwgf2umx") != std::wstring::npos)
            {
                CloseHandle(h); // NVIDIA UMD pool threads never take our locks
                continue;
            }
            cands.push_back({ te.th32ThreadID, h, ss });
        }
        CloseHandle(snap);
    };
    enumerate();
    wprintf(L"lockwatch: ui tid=%lu, %zu candidate holder threads (non-NVIDIA)\n", uiTid, cands.size());

    std::map<ULONG64, std::wstring> symCache;
    std::function<const std::wstring&(ULONG64)> symOf = [&](ULONG64 pc) -> const std::wstring& {
        auto it = symCache.find(pc);
        if (it == symCache.end())
            it = symCache.emplace(pc, symAt(pc)).first;
        return it->second;
    };
    auto walkThread = [&](HANDLE h, std::vector<ULONG64>& pcs, int maxFrames) {
        pcs.clear();
        if (SuspendThread(h) == (DWORD)-1)
            return false;
        CONTEXT wctx{};
        wctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &wctx))
        {
            STACKFRAME_EX fr{};
            fr.AddrPC.Offset = wctx.Rip; fr.AddrPC.Mode = AddrModeFlat;
            fr.AddrFrame.Offset = wctx.Rbp; fr.AddrFrame.Mode = AddrModeFlat;
            fr.AddrStack.Offset = wctx.Rsp; fr.AddrStack.Mode = AddrModeFlat;
            pcs.push_back(wctx.Rip);
            for (int f = 0; f < maxFrames; ++f)
            {
                if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, g_proc, h, &fr, &wctx, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr, SYM_STKWALK_DEFAULT))
                    break;
                if (!fr.AddrPC.Offset)
                    break;
                if (f > 0 || fr.AddrPC.Offset != pcs.front())
                    pcs.push_back(fr.AddrPC.Offset);
            }
        }
        ResumeThread(h);
        return true;
    };

    // per-thread: how many lock-wait hits it was found inside terminal code, and its collapsed stacks
    struct HolderStat
    {
        int inTerminalCode = 0;
        std::map<std::wstring, int> stacks;
        std::wstring startSym;
    };
    std::map<DWORD, HolderStat> holders;
    int waits = 0, hits = 0, longestRun = 0, run = 0;
    std::map<std::wstring, int> uiWaitSites; // which UI-side caller was waiting (top our-frame under the lock)
    std::vector<ULONG64> pcs;
    LARGE_INTEGER qpf{}, q0{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&q0);
    for (int i = 0; i < n; ++i)
    {
        if (!walkThread(hUi, pcs, 24))
        {
            Sleep(periodMs);
            continue;
        }
        const bool waiting = isLockWaitStack(pcs, symOf);
        if (!waiting)
        {
            longestRun = (std::max)(longestRun, run);
            run = 0;
            Sleep(periodMs);
            continue;
        }
        ++waits;
        ++run;
        // the UI-side site: first frame past the lock that is one of ours
        {
            std::wstring site;
            for (size_t f = 0; f < pcs.size(); ++f)
            {
                const std::wstring& s = symOf(pcs[f]);
                if (s.find(L"ticket_lock") != std::wstring::npos)
                    continue;
                if (isOurModule(s))
                {
                    site = symNoDisp(s);
                    break;
                }
            }
            uiWaitSites[site.empty() ? L"(unknown)" : site]++;
        }
        if ((hits++ % 50) == 49)
            enumerate();
        std::vector<ULONG64> tpcs;
        for (auto& c : cands)
        {
            if (!walkThread(c.h, tpcs, 20) || tpcs.empty())
                continue;
            bool inTerm = false;
            std::wstring sig;
            std::wstring lastNd;
            int frames = 0;
            for (auto pc : tpcs)
            {
                const std::wstring& s = symOf(pc);
                const bool termCode = s.rfind(L"Microsoft.Terminal.Control!", 0) == 0 || s.rfind(L"Microsoft.Terminal.Core", 0) == 0 ||
                                      s.rfind(L"TerminalConnection!", 0) == 0 || s.find(L"Render::Renderer") != std::wstring::npos ||
                                      s.find(L"Terminal::Write") != std::wstring::npos || s.find(L"AtlasEngine") != std::wstring::npos;
                if (termCode)
                    inTerm = true;
                const std::wstring nd = symNoDisp(s);
                if (frames < 12 && nd != lastNd)
                {
                    sig += nd;
                    sig += L"\n";
                    lastNd = nd;
                    ++frames;
                }
            }
            if (inTerm)
            {
                auto& hs = holders[c.tid];
                hs.inTerminalCode++;
                hs.stacks[sig]++;
                hs.startSym = c.startSym;
            }
        }
        Sleep(periodMs);
    }
    longestRun = (std::max)(longestRun, run);
    LARGE_INTEGER q1{};
    QueryPerformanceCounter(&q1);
    wprintf(L"\nlockwatch: %d samples over %.1fs; UI thread inside ticket_lock::lock in %d samples (%.1f%%); longest contiguous wait ~%.2fs\n",
            n, double(q1.QuadPart - q0.QuadPart) / double(qpf.QuadPart), waits, n ? waits * 100.0 / n : 0.0, longestRun * periodMs / 1000.0);
    wprintf(L"\n--- UI-side callers that were waiting ---\n");
    for (auto& [site, cnt] : uiWaitSites)
        wprintf(L"%5d  %s\n", cnt, site.c_str());
    wprintf(L"\n--- threads found INSIDE terminal code while the UI thread waited (the holder candidates), by hit count ---\n");
    std::vector<std::pair<DWORD, HolderStat*>> hv;
    for (auto& [tid, hs] : holders)
        hv.push_back({ tid, &hs });
    std::sort(hv.begin(), hv.end(), [](auto& a, auto& b) { return a.second->inTerminalCode > b.second->inTerminalCode; });
    int shown = 0;
    for (auto& [tid, hs] : hv)
    {
        wprintf(L"\n### tid=%lu  hits=%d  start=%s\n", tid, hs->inTerminalCode, hs->startSym.c_str());
        std::vector<std::pair<std::wstring, int>> sv(hs->stacks.begin(), hs->stacks.end());
        std::sort(sv.begin(), sv.end(), [](auto& a, auto& b) { return a.second > b.second; });
        int ss = 0;
        for (auto& [sig, cnt] : sv)
        {
            wprintf(L"  [%d]\n", cnt);
            size_t pos = 0;
            while (pos < sig.size())
            {
                size_t nl = sig.find(L'\n', pos);
                if (nl == std::wstring::npos)
                    nl = sig.size();
                wprintf(L"      %s\n", sig.substr(pos, nl - pos).c_str());
                pos = nl + 1;
            }
            if (++ss >= 3)
                break;
        }
        if (++shown >= 8)
            break;
    }
    for (auto& c : cands)
        CloseHandle(c.h);
    CloseHandle(hUi);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        wprintf(L"usage: livesample census <pid> | livesample sample <pid> <tid> <n> <periodMs> | livesample walk <pid> <tid> <n> <periodMs> [episodeMs] | livesample lockwatch <pid> <uiTid> <n> <periodMs>\n");
        return 2;
    }
    DWORD pid = (DWORD)_wtoi(argv[2]);
    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_proc)
    {
        wprintf(L"OpenProcess(%lu) failed gle=%lu\n", pid, GetLastError());
        return 1;
    }
    wchar_t symDir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LIVESAMPLE_SYMDIR", symDir, MAX_PATH))
        wcscpy_s(symDir, L"K:\\source\\Agentmaster\\src\\cascadia\\CascadiaPackage\\bin\\x64\\Release");
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitializeW(g_proc, symDir, TRUE))
        wprintf(L"SymInitializeW failed gle=%lu (symbols degraded)\n", GetLastError());

    if (wcscmp(argv[1], L"census") == 0)
        return census(pid);
    if (wcscmp(argv[1], L"sample") == 0 && argc >= 6)
        return sample(pid, (DWORD)_wtoi(argv[3]), _wtoi(argv[4]), _wtoi(argv[5]));
    if (wcscmp(argv[1], L"walk") == 0 && argc >= 6)
        return walk(pid, (DWORD)_wtoi(argv[3]), _wtoi(argv[4]), _wtoi(argv[5]), argc >= 7 ? _wtoi(argv[6]) : 500);
    if (wcscmp(argv[1], L"lockwatch") == 0 && argc >= 6)
        return lockwatch(pid, (DWORD)_wtoi(argv[3]), _wtoi(argv[4]), _wtoi(argv[5]));
    wprintf(L"bad args\n");
    return 2;
}
