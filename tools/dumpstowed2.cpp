// Agentmaster: STOWED_EXCEPTION decoder for a 0xC000027B fail-fast full dump (the crash-#7 tool).
// Based on tools/dumpstowed.cpp with three fixes/additions:
//  1. Signature check handles BOTH byte orders ('SE01' multi-char constant 0x53453031 stores
//     little-endian as bytes 31 30 45 53 — the original checked only the reversed order, so it
//     could never match a real Windows stowed-exception header).
//  2. Prints the dump's ProcessCreateTime (MiscInfoStream) — dates WHICH on-disk binary the crashed
//     process loaded (the crash-vs-fix timeline check).
//  3. Prints TerminalApp.dll / WindowsTerminal.exe TimeDateStamp from the module list — module identity.
// Plus per-candidate hexdumps so a decode failure is diagnosable, and source lines for OUR frames.
// Usage: stowed2 <dmp> <bindir-with-pdbs>
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <ctime>
#include <vector>
#include <string>

#pragma comment(lib, "dbghelp.lib")

struct Mod { ULONG64 base; ULONG32 size; ULONG32 tds; std::wstring name; std::wstring full; };
static std::vector<Mod> g_mods;
static Mod* findMod(ULONG64 a) { for (auto& m : g_mods) if (a >= m.base && a < m.base + m.size) return &m; return nullptr; }

static BYTE* g_base = nullptr;
static MINIDUMP_MEMORY64_LIST* g_ml64 = nullptr;
static MINIDUMP_MEMORY_LIST* g_ml = nullptr;

static const BYTE* readVA(ULONG64 va, size_t n) {
    if (g_ml64) {
        ULONG64 off = g_ml64->BaseRva;
        for (ULONG64 i = 0; i < g_ml64->NumberOfMemoryRanges; ++i) {
            auto& r = g_ml64->MemoryRanges[i];
            if (va >= r.StartOfMemoryRange && va + n <= r.StartOfMemoryRange + r.DataSize)
                return g_base + off + (va - r.StartOfMemoryRange);
            off += r.DataSize;
        }
    }
    if (g_ml) {
        for (ULONG32 i = 0; i < g_ml->NumberOfMemoryRanges; ++i) {
            auto& r = g_ml->MemoryRanges[i];
            if (va >= r.StartOfMemoryRange && va + n <= r.StartOfMemoryRange + r.Memory.DataSize)
                return g_base + r.Memory.Rva + (va - r.StartOfMemoryRange);
        }
    }
    return nullptr;
}

static void symLine(ULONG64 addr) {
    Mod* m = findMod(addr);
    const wchar_t* mn = m ? m->name.c_str() : L"?";
    ULONG64 rva = m ? (addr - m->base) : 0;
    char buf[sizeof(SYMBOL_INFO) + 1024];
    auto* si = (SYMBOL_INFO*)buf; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 1000;
    DWORD64 disp = 0;
    bool ours = m && (_wcsicmp(m->name.c_str(), L"TerminalApp.dll") == 0);
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, si)) {
        wprintf(L"    %s0x%llX  %-22s +0x%-8llX %S +0x%llX",
                ours ? L">>OURS<< " : L"", (unsigned long long)addr, mn, (unsigned long long)rva, si->Name, (unsigned long long)disp);
        if (ours) { // source line for our module
            IMAGEHLP_LINEW64 line{ sizeof(IMAGEHLP_LINEW64) }; DWORD ld = 0;
            if (SymGetLineFromAddrW64(GetCurrentProcess(), addr, &ld, &line))
                wprintf(L"  [%s:%u]", wcsrchr(line.FileName, L'\\') ? wcsrchr(line.FileName, L'\\') + 1 : line.FileName, line.LineNumber);
        }
        wprintf(L"\n");
    } else {
        wprintf(L"    %s0x%llX  %-22s +0x%llX\n", ours ? L">>OURS<< " : L"", (unsigned long long)addr, mn, (unsigned long long)rva);
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: stowed2 <dmp> <bindir>\n"); return 2; }
    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
    g_base = (BYTE*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!g_base) { wprintf(L"map failed\n"); return 1; }

    void* s = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;

    // Process create time — dates which on-disk binary the process loaded (timeline check).
    if (MiniDumpReadDumpStream(g_base, MiscInfoStream, &dir, &s, &sz)) {
        auto* mi = (MINIDUMP_MISC_INFO*)s;
        if (mi->Flags1 & MINIDUMP_MISC1_PROCESS_TIMES) {
            time_t ct = (time_t)mi->ProcessCreateTime;
            struct tm tmv; localtime_s(&tmv, &ct);
            wchar_t tb[64]; wcsftime(tb, 64, L"%Y-%m-%d %H:%M:%S", &tmv);
            wprintf(L"ProcessCreateTime = %s (local)   pid=%u\n", tb, mi->ProcessId);
        }
    }

    if (MiniDumpReadDumpStream(g_base, ModuleListStream, &dir, &s, &sz)) {
        auto* ml = (MINIDUMP_MODULE_LIST*)s;
        for (ULONG32 i = 0; i < ml->NumberOfModules; ++i) {
            auto& m = ml->Modules[i];
            auto* ms = (MINIDUMP_STRING*)(g_base + m.ModuleNameRva);
            std::wstring full(ms->Buffer, ms->Length / 2);
            size_t slash = full.find_last_of(L"\\/");
            g_mods.push_back({ m.BaseOfImage, m.SizeOfImage, m.TimeDateStamp, slash == std::wstring::npos ? full : full.substr(slash + 1), full });
        }
    }
    // Module identity for the interesting binaries.
    for (auto& m : g_mods) {
        if (_wcsicmp(m.name.c_str(), L"TerminalApp.dll") == 0 || _wcsicmp(m.name.c_str(), L"WindowsTerminal.exe") == 0) {
            time_t ts = (time_t)m.tds;
            struct tm tmv; localtime_s(&tmv, &ts);
            wchar_t tb[64]; wcsftime(tb, 64, L"%Y-%m-%d %H:%M:%S", &tmv);
            wprintf(L"MODULE %-20s TimeDateStamp=0x%08X (%s local)  path=%s\n", m.name.c_str(), m.tds, tb, m.full.c_str());
        }
    }
    wprintf(L"\n");

    if (MiniDumpReadDumpStream(g_base, Memory64ListStream, &dir, &s, &sz)) g_ml64 = (MINIDUMP_MEMORY64_LIST*)s;
    if (MiniDumpReadDumpStream(g_base, MemoryListStream, &dir, &s, &sz)) g_ml = (MINIDUMP_MEMORY_LIST*)s;

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS);
    SymInitializeW(GetCurrentProcess(), argv[2], FALSE);
    for (auto& m : g_mods)
        SymLoadModuleExW(GetCurrentProcess(), nullptr, m.full.c_str(), m.name.c_str(), m.base, m.size, nullptr, 0);

    if (!MiniDumpReadDumpStream(g_base, ExceptionStream, &dir, &s, &sz)) { wprintf(L"no exception stream\n"); return 1; }
    auto* es = (MINIDUMP_EXCEPTION_STREAM*)s;
    auto& er = es->ExceptionRecord;
    wprintf(L"ExceptionCode=0x%08X  faulting tid=0x%X  params=%u\n", er.ExceptionCode, es->ThreadId, er.NumberParameters);
    if (er.NumberParameters < 1) { wprintf(L"no stowed pointer\n"); return 1; }

    ULONG64 p0 = er.ExceptionInformation[0];
    ULONG64 count = er.NumberParameters >= 2 ? er.ExceptionInformation[1] : 1;
    wprintf(L"stowed array @0x%llX  count=%llu\n\n", (unsigned long long)p0, (unsigned long long)count);
    if (count > 32) count = 32;

    std::vector<ULONG64> cands;
    if (const BYTE* arr = readVA(p0, (size_t)count * 8)) {
        for (ULONG64 i = 0; i < count; ++i) { ULONG64 v; memcpy(&v, arr + i * 8, 8); if (v > 0x10000) cands.push_back(v); }
    } else {
        cands.push_back(p0); // maybe [0] IS the struct
    }

    bool decodedAny = false;
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        ULONG64 sp = cands[ci];
        const BYTE* p = readVA(sp, 56);
        wprintf(L"--- candidate[%zu] @0x%llX: %s\n", ci, (unsigned long long)sp, p ? L"" : L"<memory not in dump>");
        if (!p) continue;
        wprintf(L"    raw: "); for (int i = 0; i < 56; ++i) wprintf(L"%02X ", p[i]); wprintf(L"\n");
        ULONG32 Size; memcpy(&Size, p + 0, 4);
        ULONG32 Sig;  memcpy(&Sig, p + 4, 4);
        char lo[5] = { (char)(Sig & 0xFF), (char)((Sig >> 8) & 0xFF), (char)((Sig >> 16) & 0xFF), (char)((Sig >> 24) & 0xFF), 0 };
        // 'SE01' = 0x53453031 -> memory bytes 31 30 45 53 (lo = "10ES"); accept either rendering.
        bool sig = (lo[0] == 'S' && lo[1] == 'E' && lo[2] == '0') || (lo[3] == 'S' && lo[2] == 'E' && lo[1] == '0');
        if (!sig) { wprintf(L"    (no SE0x signature: Size=%u Sig=0x%08X '%S')\n", Size, Sig, lo); continue; }
        decodedAny = true;
        LONG hr; memcpy(&hr, p + 8, 4);
        ULONG32 bits; memcpy(&bits, p + 12, 4);
        ULONG32 form = bits & 0x3;
        ULONG32 tid = bits >> 2;
        wprintf(L"    STOWED  Size=%u Sig=0x%08X  ResultCode=0x%08X  form=%u  tid=0x%X\n", Size, Sig, (unsigned)hr, form, tid);
        if (form == 2) {
            ULONG64 txt; memcpy(&txt, p + 16, 8);
            if (const BYTE* t = readVA(txt, 512)) wprintf(L"    ErrorText = %.255s\n", (const wchar_t*)t);
        } else {
            ULONG64 exAddr; memcpy(&exAddr, p + 16, 8);
            ULONG32 wordSz; memcpy(&wordSz, p + 24, 4);
            ULONG32 words; memcpy(&words, p + 28, 4);
            ULONG64 traceP; memcpy(&traceP, p + 32, 8);
            if (exAddr) { wprintf(L"    ExceptionAddress:\n"); symLine(exAddr); }
            wprintf(L"    Backtrace (%u frames, wordSize=%u) @0x%llX:\n", words, wordSz, (unsigned long long)traceP);
            if (words > 256) words = 256;
            const BYTE* tb = readVA(traceP, (size_t)words * (wordSz ? wordSz : 8));
            if (tb) {
                for (ULONG32 i = 0; i < words; ++i) {
                    ULONG64 a = 0; memcpy(&a, tb + (size_t)i * (wordSz ? wordSz : 8), wordSz == 4 ? 4 : 8);
                    if (a) symLine(a);
                }
            } else wprintf(L"    <backtrace memory not in dump>\n");
        }
        if (Size >= 56) { // V2 nested chain
            ULONG32 nestType; memcpy(&nestType, p + 40, 4);
            ULONG64 nest; memcpy(&nest, p + 48, 8);
            if (nest && nestType != 0) { wprintf(L"    NestedExceptionType=0x%X NestedException=0x%llX (chained)\n", nestType, (unsigned long long)nest); cands.push_back(nest); }
        }
        wprintf(L"\n");
    }
    if (!decodedAny) wprintf(L"No STOWED_EXCEPTION signature found.\n");
    return 0;
}
