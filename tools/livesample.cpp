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

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        wprintf(L"usage: livesample census <pid> | livesample sample <pid> <tid> <n> <periodMs>\n");
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
    wprintf(L"bad args\n");
    return 2;
}
